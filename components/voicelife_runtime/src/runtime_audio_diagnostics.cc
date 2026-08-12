#include "runtime_audio_diagnostics.h"

#ifdef ESP_PLATFORM

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "voicelife/audio_esp/audio_board_profile.h"
#include "voicelife/audio_esp/esp32s3_pcm_audio_port.h"
#include "voicelife/voice/voice_ports.h"

namespace voicelife::runtime {

#if CONFIG_VOICELIFE_AUDIO_PORT_SMOKE
Status RunVoiceLifePcbAudioPortSmoke() {
    constexpr char kTag[] = "VoiceLifeRuntime";
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
    if (!status.ok()) return status;
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
    for (std::size_t index = 0; index < tone_samples; ++index) {
        const int16_t sample = (index / 24U) % 2U == 0U ? 1200 : -1200;
        std::memcpy(tone.payload.data() + index * sizeof(sample), &sample, sizeof(sample));
    }
    status = ports.output().Push(tone);
    if (status.ok()) vTaskDelay(pdMS_TO_TICKS(150));

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
    if (!status.ok()) return status;
    if (captured_frames.load() == 0 || nonzero_samples.load() == 0) {
        return Status::Error(ErrorCode::kUnavailable, "PCM Audio Port 未检测到可变化的总线输入");
    }
    if (stats.played_frames == 0) {
        return Status::Error(ErrorCode::kUnavailable, "PCM Audio Port 未完成总线回放帧");
    }
    return Status::Ok();
}
#else
Status RunVoiceLifePcbAudioPortSmoke() { return Status::Error(ErrorCode::kUnavailable, "PCM Audio Port 自检未启用"); }
#endif

}  // namespace voicelife::runtime

#endif
