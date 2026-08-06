#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "voicelife/audio_esp/audio_board_profile.h"

namespace voicelife::audio_esp {

struct AudioProbeReport {
    bool codec_control_required = false;
    bool i2c_bus_ready = false;
    bool es8311_ack = false;
    bool es7210_ack = false;
    bool pca9557_ack = false;
    bool i2s_channels_ready = false;
    bool i2s_channels_started = false;
    std::size_t bytes_written = 0;
    std::size_t bytes_read = 0;
    std::size_t replay_bytes_written = 0;
    std::size_t capture_samples = 0;
    std::size_t nonzero_samples = 0;
    std::size_t changed_samples = 0;
    std::size_t saturated_samples = 0;
    uint32_t peak_abs = 0;
    uint64_t sum_squares = 0;
    std::size_t minimum_free_heap_bytes = 0;

    [[nodiscard]] bool hardware_ready() const {
        const bool codec_ready = !codec_control_required ||
                                 (i2c_bus_ready && es8311_ack && es7210_ack && pca9557_ack);
        return codec_ready && i2s_channels_ready && i2s_channels_started;
    }

    [[nodiscard]] uint64_t mean_square() const {
        return capture_samples == 0 ? 0 : sum_squares / capture_samples;
    }

    [[nodiscard]] uint64_t saturation_ratio_ppm() const {
        return capture_samples == 0 ? 0 : saturated_samples * 1000000ULL / capture_samples;
    }

    // This is digital input evidence only. It deliberately does not claim
    // that the speaker was heard; that needs an external acoustic observer.
    [[nodiscard]] bool capture_signal_detected() const {
        return capture_samples >= 160 && nonzero_samples >= capture_samples / 20 &&
               changed_samples >= capture_samples / 100 && peak_abs >= 32 &&
               mean_square() >= 256;
    }
};

struct AudioProbeOptions {
    uint32_t timeout_ms = 200;
    uint32_t capture_duration_ms = 300;
    bool replay_capture = false;
};

// A bounded board probe. It verifies the selected topology, I2S DMA lifecycle,
// logical PCM evidence and (optionally) one attenuated replay frame. It does
// not initialize codec registers or claim acoustic playback without an
// external observer.
class Esp32s3AudioProbe final {
   public:
    Esp32s3AudioProbe();
    ~Esp32s3AudioProbe();

    Esp32s3AudioProbe(const Esp32s3AudioProbe&) = delete;
    Esp32s3AudioProbe& operator=(const Esp32s3AudioProbe&) = delete;

    Result<AudioProbeReport> Run(const AudioBoardProfile& profile,
                                 const AudioProbeOptions& options = {});

   private:
    /** @brief Pimpl 实现。 */
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace voicelife::audio_esp
