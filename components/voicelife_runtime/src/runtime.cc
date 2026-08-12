#include "voicelife/runtime/runtime.h"

#include <memory>
#include <string>
#include <utility>

#ifdef ESP_PLATFORM
#include <atomic>
#include <cstdint>
#include <cstring>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "voicelife/audio_esp/audio_board_profile.h"
#include "voicelife/audio_esp/esp32s3_audio_probe.h"
#include "voicelife/audio_esp/esp32s3_pcm_audio_port.h"
#endif

#include "bootstrap/storage_bootstrap.h"
#include "voicelife/voice/voice_ports.h"
#include "voicelife/voice/voice_session.h"

namespace voicelife::runtime {
namespace {

#ifdef ESP_PLATFORM
constexpr char kTag[] = "VoiceLifeRuntime";

#if CONFIG_VOICELIFE_AUDIO_PORT_SMOKE
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
#endif

class ScaffoldAudioInput final : public voice::AudioInputPort {
   public:
    void SetAudioSink(voice::AudioFrameSink) override {}
    Status Open(const voice::AudioFormat&) override { return Status::Ok(); }
    Status StartCapture(voice::VoiceMode) override { return Status::Ok(); }
    Status StopCapture() override { return Status::Ok(); }
    void Close() override {}
};

class ScaffoldAudioOutput final : public voice::AudioOutputPort {
   public:
    Status Open(const voice::AudioFormat&) override { return Status::Ok(); }
    Status Push(const voice::AudioFrame&) override { return Status::Ok(); }
    Status Flush() override { return Status::Ok(); }
    void Close() override {}
};

class ScaffoldSpeechProvider final : public voice::SpeechProviderAdapter {
   public:
    Status Connect(const voice::VoiceSessionConfig&, voice::VoiceEventSink) override { return Status::Ok(); }
    Status StartCapture(voice::VoiceMode) override { return Status::Ok(); }
    Status StopCapture() override { return Status::Ok(); }
    Status SendAudio(const voice::AudioFrame&) override { return Status::Ok(); }
    Status Abort(std::string_view) override { return Status::Ok(); }
    Status Speak(std::string_view) override { return Status::Ok(); }
    Status Disconnect() override { return Status::Ok(); }
    Result<voice::VoiceAudioFormats> audio_formats() const override {
        voice::VoiceAudioFormats fmt;
        fmt.capture = voice::AudioFormat{};
        fmt.playback = voice::AudioFormat{};
        return Result<voice::VoiceAudioFormats>::Success(fmt);
    }
    const voice::CapabilityProfile& capabilities() const override { return profile_; }

   private:
    voice::CapabilityProfile profile_{"scaffold", {"streaming-asr", "tts"}};
};

class Runtime final {
   public:
    Runtime() {
        auto& registry = voice::SpeechProviderRegistry::Instance();
        registry.Register("scaffold", voice::CapabilityProfile{"scaffold", {"streaming-asr", "tts"}},
                          []() { return std::make_unique<ScaffoldSpeechProvider>(); });
    }

    Status Start() {
        if (session_ != nullptr) {
            return Status::Error(ErrorCode::kConflict, "VoiceLife Runtime 已经启动");
        }

        const Status storage_status = storage_.Start();
        if (!storage_status.ok()) return storage_status;

        auto& registry = voice::SpeechProviderRegistry::Instance();
        auto result = registry.Create("scaffold", {});
        if (!result.ok() || !result.value.has_value()) {
            return FailStart(Status::Error(ErrorCode::kInternal, "无法创建语音 Provider: " + result.status.message));
        }
        provider_ = std::move(*result.value);

        session_ = std::make_unique<voice::VoiceSession>(audio_input_, audio_output_, *provider_);

        voice::VoiceSessionConfig config;
        config.session_id = "scaffold-session";
        config.provider_id = "scaffold";
        const Status session_status = session_->Start(config);
        if (!session_status.ok()) {
            return FailStart(session_status);
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
        const auto probe_result = probe.Run(profile, options);
        if (!probe_result.ok() || !probe_result.value.has_value()) {
            return FailStart(probe_result.status);
        }
        const auto& report = *probe_result.value;
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
            return FailStart(Status::Error(ErrorCode::kUnavailable, "音频探针硬件未就绪，Codec ACK 或 I2S 状态不完整"));
        }
        if (!report.capture_signal_detected()) {
            return FailStart(Status::Error(ErrorCode::kUnavailable, "音频探针未检测到可变化的 PCM 输入"));
        }
#endif
#if defined(ESP_PLATFORM) && CONFIG_VOICELIFE_AUDIO_PORT_SMOKE
        const Status audio_port_status = RunAudioPortSmoke();
        if (!audio_port_status.ok()) {
            return FailStart(audio_port_status);
        }
#endif
        return Status::Ok();
    }

   private:
    /**
     * @brief 回滚一次未完成的 Runtime 启动。
     * @param failure 首个失败阶段返回的状态。
     * @return 保留首个失败原因，并在必要时附加清理失败信息。
     */
    Status FailStart(Status failure) {
        if (session_ != nullptr) {
            const Status stop_status = session_->Stop();
            if (!stop_status.ok()) {
                failure.message += "；语音会话清理失败：" + stop_status.message;
            }
        }
        session_.reset();
        provider_.reset();
        const Status storage_stop_status = storage_.Stop();
        if (!storage_stop_status.ok()) {
            failure.message += "；存储清理失败：" + storage_stop_status.message;
        }
        return failure;
    }

    // 先声明存储，使析构顺序为语音资源、SQLite、FATFS 数据卷。
    StorageBootstrap storage_;
    ScaffoldAudioInput audio_input_;
    ScaffoldAudioOutput audio_output_;
    std::unique_ptr<voice::SpeechProviderAdapter> provider_;
    std::unique_ptr<voice::VoiceSession> session_;
};

}  // namespace

Status Start() {
    static Runtime runtime;
    return runtime.Start();
}

}  // namespace voicelife::runtime
