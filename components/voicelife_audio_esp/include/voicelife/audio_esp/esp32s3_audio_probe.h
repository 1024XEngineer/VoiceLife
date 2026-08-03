#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "voicelife/audio_esp/audio_board_profile.h"

namespace voicelife::audio_esp {

struct AudioProbeReport {
    bool i2c_bus_ready = false;
    bool es8311_ack = false;
    bool es7210_ack = false;
    bool pca9557_ack = false;
    bool i2s_channels_ready = false;
    bool i2s_channels_started = false;
    std::size_t bytes_written = 0;
    std::size_t bytes_read = 0;

    [[nodiscard]] bool hardware_ready() const {
        return i2c_bus_ready && es8311_ack && es7210_ack && pca9557_ack &&
               i2s_channels_ready && i2s_channels_started;
    }
};

// A bounded, offline board probe. It verifies pin/address wiring and I2S DMA
// lifecycle only; it deliberately does not enable the speaker PA or initialize
// ES8311/ES7210 registers, so a passing probe is not an audio loopback result.
class Esp32s3AudioProbe final {
   public:
    Esp32s3AudioProbe();
    ~Esp32s3AudioProbe();

    Esp32s3AudioProbe(const Esp32s3AudioProbe&) = delete;
    Esp32s3AudioProbe& operator=(const Esp32s3AudioProbe&) = delete;

    Result<AudioProbeReport> Run(const AudioBoardProfile& profile,
                                 uint32_t timeout_ms = 100);

   private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace voicelife::audio_esp
