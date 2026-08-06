#include "voicelife/runtime/runtime.h"

#ifdef ESP_PLATFORM
#include "esp_log.h"
#endif

#include "voicelife/audio_esp/audio_board_profile.h"
#include "voicelife/audio_esp/esp32s3_audio_probe.h"
#include "voicelife/mcp/mcp_tool_gateway.h"
#include "voicelife/voice/voice_session_coordinator.h"

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
