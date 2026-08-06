#include "voicelife/runtime/runtime.h"

#include <memory>
#include <string>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "voicelife/audio_esp/audio_board_profile.h"
#include "voicelife/audio_esp/esp32s3_audio_probe.h"
#endif

#include "voicelife/voice/voice_ports.h"
#include "voicelife/voice/voice_session.h"

namespace voicelife::runtime {
namespace {

#ifdef ESP_PLATFORM
constexpr char kTag[] = "VoiceLifeRuntime";
#endif

// No-op adapters keep the composition root runnable until a platform profile
// supplies real audio and speech implementations.
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
        auto& registry = voice::SpeechProviderRegistry::Instance();
        auto result = registry.Create("scaffold", {});
        if (!result.ok() || !result.value.has_value()) {
            return Status::Error(ErrorCode::kInternal, "无法创建语音 Provider: " + result.status.message);
        }
        provider_ = std::move(*result.value);

        session_ = std::make_unique<voice::VoiceSession>(audio_input_, audio_output_, *provider_);

        voice::VoiceSessionConfig config;
        config.session_id = "scaffold-session";
        config.provider_id = "scaffold";
        const Status session_status = session_->Start(config);
        if (!session_status.ok()) {
            return session_status;
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
            return probe_result.status;
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
            return Status::Error(ErrorCode::kUnavailable, "音频探针硬件未就绪，Codec ACK 或 I2S 状态不完整");
        }
        if (!report.capture_signal_detected()) {
            return Status::Error(ErrorCode::kUnavailable, "音频探针未检测到可变化的 PCM 输入");
        }
#endif
        return Status::Ok();
    }

   private:
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
