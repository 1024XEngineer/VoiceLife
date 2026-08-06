#include "voicelife/runtime/runtime.h"

#ifdef ESP_PLATFORM
#include "esp_log.h"
#endif

#include "voicelife/audio_esp/audio_board_profile.h"
#include "voicelife/audio_esp/esp32s3_audio_probe.h"
#include "voicelife/audio_esp/esp32s3_pcm_audio_port.h"
#include "voicelife/mcp/mcp_tool_gateway.h"
#include "voicelife/voice/voice_session_coordinator.h"

#ifdef ESP_PLATFORM
#include <atomic>
#include <cstring>
#include <vector>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

namespace voicelife::runtime {
namespace {

#ifdef ESP_PLATFORM
constexpr char kTag[] = "VoiceLifeRuntime";
#endif

// 提供可启动的音频设备占位适配器。
class ScaffoldAudioAdapter final : public voice::AudioDevicePort {
   public:
    Status Open() override { return Status::Ok(); }
    void Close() override {}
};

// 提供可连接的语音服务占位适配器。
class ScaffoldSpeechAdapter final : public voice::SpeechProviderPort {
   public:
    Status Connect() override { return Status::Ok(); }
    void Disconnect() override {}
};

// 将语音工具调用转发给通用 MCP 注册中心。
class McpVoiceBridge final : public voice::ToolGatewayPort {
   public:
    explicit McpVoiceBridge(mcp::McpToolGateway& gateway) : gateway_(gateway) {}
    ToolResult Call(const ToolCall& call) override { return gateway_.call(call); }

   private:
    mcp::McpToolGateway& gateway_;
};

#if defined(ESP_PLATFORM) && CONFIG_VOICELIFE_AUDIO_PORT_SMOKE

Status RunAudioPortSmoke() {
    const auto profile = audio_esp::VoiceLifePcbEsp32s3Profile();
    audio_esp::Esp32s3PcmAudioPorts ports(profile);
    std::atomic<std::size_t> captured_frames{0};
    std::atomic<std::size_t> nonzero_samples{0};
    ports.input().SetAudioSink([&](voice::AudioFrame frame) {
        captured_frames.fetch_add(1);
        for (std::size_t offset = 0; offset + 1 < frame.payload.size(); offset += 2) {
            if (frame.payload[offset] != 0 || frame.payload[offset + 1] != 0) {
                nonzero_samples.fetch_add(1);
            }
        }
        return Status::Ok();
    });

    auto capture_format = profile.capture_i2s.format;
    capture_format.frame_duration_ms = 60;
    auto playback_format = profile.playback_i2s.format;
    playback_format.frame_duration_ms = 60;

    Status status = ports.input().Open(capture_format);
    if (!status.ok()) {
        return status;
    }
    status = ports.output().Open(playback_format);
    if (!status.ok()) {
        ports.input().Close();
        return status;
    }
    status = ports.input().StartCapture(voice::VoiceMode::kManual);
    if (!status.ok()) {
        ports.output().Close();
        ports.input().Close();
        return status;
    }

    vTaskDelay(pdMS_TO_TICKS(300));
    status = ports.input().StopCapture();
    if (!status.ok()) {
        ports.output().Close();
        ports.input().Close();
        return status;
    }

    voice::AudioFrame tone;
    tone.format = playback_format;
    const std::size_t tone_samples =
        static_cast<std::size_t>(playback_format.sample_rate_hz) * playback_format.frame_duration_ms / 1000U;
    tone.payload.resize(tone_samples * sizeof(int16_t));
    for (std::size_t i = 0; i < tone_samples; ++i) {
        const int16_t sample = (i / 24U) % 2U == 0U ? 1200 : -1200;
        std::memcpy(tone.payload.data() + i * sizeof(sample), &sample, sizeof(sample));
    }
    status = ports.output().Push(tone);
    if (status.ok()) {
        vTaskDelay(pdMS_TO_TICKS(150));
    }

    const auto stats = ports.stats();
    ESP_LOGI(kTag,
             "AUDIO_PORT_READY=1 AUDIO_PORT_CAPTURE_FRAMES=%u AUDIO_PORT_PLAYED_FRAMES=%u "
             "AUDIO_PORT_DROPPED_INPUT=%u AUDIO_PORT_REJECTED_OUTPUT=%u "
             "AUDIO_PORT_SHORT_READS=%u AUDIO_PORT_SHORT_WRITES=%u "
             "AUDIO_PORT_MIN_HEAP=%u AUDIO_PORT_SIGNAL=%d",
             static_cast<unsigned>(captured_frames.load()), static_cast<unsigned>(stats.played_frames),
             static_cast<unsigned>(stats.dropped_input_frames), static_cast<unsigned>(stats.rejected_output_frames),
             static_cast<unsigned>(stats.short_reads), static_cast<unsigned>(stats.short_writes),
             static_cast<unsigned>(stats.minimum_free_heap_bytes), nonzero_samples.load() > 0);

    ports.output().Close();
    ports.input().Close();
    if (!status.ok()) {
        return status;
    }
    if (captured_frames.load() == 0 || nonzero_samples.load() == 0) {
        return Status::Error(ErrorCode::kUnavailable, "PCM Audio Port 未检测到可变化的总线输入");
    }
    if (stats.played_frames == 0) {
        return Status::Error(ErrorCode::kUnavailable, "PCM Audio Port 未完成总线回放帧");
    }
    return Status::Ok();
}

#endif

// 组装当前可用的语音和 MCP 基础能力。
class Runtime final {
   public:
    Runtime() : mcp_voice_bridge_(mcp_), voice_(audio_, speech_, mcp_voice_bridge_) {}
    Status Start() {
        const Status voice_status = voice_.Start();
        if (!voice_status.ok()) {
            return voice_status;
        }

#if defined(ESP_PLATFORM) && CONFIG_VOICELIFE_AUDIO_PROBE
        audio_esp::Esp32s3AudioProbe probe;
        const auto profile =
#if CONFIG_VOICELIFE_AUDIO_PROBE_PROFILE_VOICELIFE_PCB
            audio_esp::VoiceLifePcbEsp32s3Profile();
#else
            audio_esp::LichuangEsp32s3Profile();
#endif
        audio_esp::AudioProbeOptions options;
#if CONFIG_VOICELIFE_AUDIO_PROBE_REPLAY
        options.replay_capture = true;
#endif
        const auto result = probe.Run(profile, options);
        if (!result.ok() || !result.value.has_value()) {
            return result.status;
        }
        const auto& report = *result.value;
        ESP_LOGI(kTag,
                 "音频探针：profile=%s codec_required=%d I2C=%d ES8311=%d ES7210=%d "
                 "PCA9557=%d I2S_READY=%d I2S_STARTED=%d bus_write=%u bus_read=%u "
                 "pcm_samples=%u nonzero=%u changed=%u saturated=%u saturation_ppm=%llu "
                 "peak=%u mean_square=%llu signal=%d replay=%u min_heap=%u",
                 profile.id.c_str(), report.codec_control_required, report.i2c_bus_ready, report.es8311_ack,
                 report.es7210_ack, report.pca9557_ack, report.i2s_channels_ready, report.i2s_channels_started,
                 static_cast<unsigned>(report.bytes_written), static_cast<unsigned>(report.bytes_read),
                 static_cast<unsigned>(report.capture_samples), static_cast<unsigned>(report.nonzero_samples),
                 static_cast<unsigned>(report.changed_samples), static_cast<unsigned>(report.saturated_samples),
                 static_cast<unsigned long long>(report.saturation_ratio_ppm()), static_cast<unsigned>(report.peak_abs),
                 static_cast<unsigned long long>(report.mean_square()), report.capture_signal_detected(),
                 static_cast<unsigned>(report.replay_bytes_written),
                 static_cast<unsigned>(report.minimum_free_heap_bytes));
        if (!report.hardware_ready()) {
            return Status::Error(ErrorCode::kUnavailable, "音频探针硬件未就绪，Codec ACK 或 I2S 状态不完整");
        }
        if (!report.capture_signal_detected()) {
            return Status::Error(ErrorCode::kUnavailable, "音频探针未检测到可变化的 PCM 输入");
        }
#endif
#if defined(ESP_PLATFORM) && CONFIG_VOICELIFE_AUDIO_PORT_SMOKE
        const Status audio_port_status = RunAudioPortSmoke();
        if (!audio_port_status.ok()) {
            return audio_port_status;
        }
#endif
        return Status::Ok();
    }

   private:
    mcp::McpToolGateway mcp_;
    McpVoiceBridge mcp_voice_bridge_;
    ScaffoldAudioAdapter audio_;
    ScaffoldSpeechAdapter speech_;
    voice::VoiceSessionCoordinator voice_;
};

}  // namespace

// 启动全局运行时实例。
Status Start() {
    static Runtime runtime;
    return runtime.Start();
}

}  // namespace voicelife::runtime
