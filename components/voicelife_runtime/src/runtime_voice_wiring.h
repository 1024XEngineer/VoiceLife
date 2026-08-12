#pragma once

#ifdef ESP_PLATFORM

#include <functional>
#include <memory>
#include <string_view>

#include "voicelife/audio_esp/esp32s3_pcm_audio_port.h"
#include "voicelife/audio_esp/esp_multinet_wake_detector.h"
#include "voicelife/voice/voice_session.h"
#include "voicelife/voice/wake_gate_audio_input.h"

namespace voicelife::runtime {

/**
 * @brief VoiceLife PCB 的语音硬件组合根。
 *
 * 保留旧板已验证的音频、唤醒和会话装配顺序。其他板卡应提供并列的
 * 组合根，而不是修改此类中的硬件 Profile。
 */
class VoiceLifePcbVoiceWiring final {
   public:
    /** @brief 为已创建的语音 Provider 装配旧板语音会话。 */
    void Assemble(voice::SpeechProviderAdapter& provider, voice::EvidenceSink evidence_sink,
                  voice::WakeGateAudioInput::WakeSink wake_sink, int volume);
    /** @brief 启动已装配的旧板语音会话。 */
    Status StartSession();

    /** @brief 返回已启动的语音会话；Start() 前为 nullptr。 */
    [[nodiscard]] voice::VoiceSession* session() const { return session_.get(); }
    /** @brief 返回旧板本地唤醒门；Start() 前为 nullptr。 */
    [[nodiscard]] voice::WakeGateAudioInput* wake_gate() const { return wake_gate_.get(); }
    /** @brief 返回旧板音频端口；Start() 前为 nullptr。 */
    [[nodiscard]] audio_esp::Esp32s3PcmAudioPorts* audio_ports() const { return audio_ports_.get(); }
    /** @brief 返回端口统计；未装配时返回空统计。 */
    [[nodiscard]] audio_esp::AudioPortStats audio_stats() const;
    /** @brief 设置旧板播放音量。 */
    void SetOutputVolume(int volume);

   private:
    std::unique_ptr<audio_esp::Esp32s3PcmAudioPorts> audio_ports_;
    std::unique_ptr<audio_esp::EspMultiNetWakeDetector> wake_detector_;
    std::unique_ptr<voice::WakeGateAudioInput> wake_gate_;
    std::unique_ptr<voice::VoiceSession> session_;
};

}  // namespace voicelife::runtime

#endif
