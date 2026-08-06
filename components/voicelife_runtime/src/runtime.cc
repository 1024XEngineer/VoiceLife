#include "voicelife/runtime/runtime.h"

#include <memory>
#include <string>

#include "voicelife/voice/voice_ports.h"
#include "voicelife/voice/voice_session.h"

namespace voicelife::runtime {
namespace {

// ---- Scaffold adapters for VoiceSession ports ----
// Safe no-op implementations that let the architecture compile, link and
// start without real hardware. Each is replaced by a real adapter when the
// platform component (ESP32 I2S, Linx WSS, etc.) is linked and registered
// through SpeechProviderRegistry.

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
        return session_->Start(config);
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
