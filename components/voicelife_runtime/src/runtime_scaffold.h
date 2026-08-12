#pragma once

#include <string_view>

#include "voicelife/voice/voice_ports.h"

namespace voicelife::runtime {

/** @brief 仅用于非 ESP 主机串联的空音频输入适配器。 */
class ScaffoldAudioInput final : public voice::AudioInputPort {
   public:
    void SetAudioSink(voice::AudioFrameSink) override {}
    Status Open(const voice::AudioFormat&) override { return Status::Ok(); }
    Status StartCapture(voice::VoiceMode) override { return Status::Ok(); }
    Status StopCapture() override { return Status::Ok(); }
    void Close() override {}
};

/** @brief 仅用于非 ESP 主机串联的空音频输出适配器。 */
class ScaffoldAudioOutput final : public voice::AudioOutputPort {
   public:
    Status Open(const voice::AudioFormat&) override { return Status::Ok(); }
    Status Push(const voice::AudioFrame&) override { return Status::Ok(); }
    Status Flush() override { return Status::Ok(); }
    bool IsIdle() const override { return true; }
    void Close() override {}
};

/** @brief 仅用于非 ESP 主机串联的空语音 Provider。 */
class ScaffoldSpeechProvider final : public voice::SpeechProviderAdapter {
   public:
    Status Connect(const voice::VoiceSessionConfig&, voice::VoiceEventSink) override { return Status::Ok(); }
    Status StartCapture(voice::VoiceMode) override { return Status::Ok(); }
    Status StopCapture() override { return Status::Ok(); }
    Status SendAudio(const voice::AudioFrame&) override { return Status::Ok(); }
    Status Abort(std::string_view) override { return Status::Ok(); }
    Status Speak(std::string_view) override { return Status::Ok(); }
    Status NotifyLocalWakeWord(std::string_view) override { return Status::Ok(); }
    Status Disconnect() override { return Status::Ok(); }
    Result<voice::VoiceAudioFormats> audio_formats() const override {
        voice::VoiceAudioFormats formats;
        formats.capture = voice::AudioFormat{};
        formats.playback = voice::AudioFormat{};
        return Result<voice::VoiceAudioFormats>::Success(formats);
    }
    const voice::CapabilityProfile& capabilities() const override { return capabilities_; }

   private:
    voice::CapabilityProfile capabilities_{"scaffold", {"streaming-asr", "tts"}};
};

}  // namespace voicelife::runtime
