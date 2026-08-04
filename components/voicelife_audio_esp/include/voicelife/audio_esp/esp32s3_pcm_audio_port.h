#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "voicelife/audio_esp/audio_board_profile.h"
#include "voicelife/voice/voice_ports.h"

namespace voicelife::audio_esp {

struct AudioPortOptions {
    uint32_t io_timeout_ms = 100;
    std::size_t input_queue_depth = 4;
    std::size_t output_queue_depth = 4;
};

struct AudioPortStats {
    std::size_t captured_frames = 0;
    std::size_t dropped_input_frames = 0;
    std::size_t played_frames = 0;
    std::size_t rejected_output_frames = 0;
    std::size_t short_reads = 0;
    std::size_t short_writes = 0;
    std::size_t input_high_watermark = 0;
    std::size_t output_high_watermark = 0;
    std::size_t minimum_free_heap_bytes = 0;
};

// Owns one profile-driven RX/TX pair and exposes two platform-neutral Ports.
// The shared owner is important for full-duplex Codec profiles: both channels
// must be initialized and released as one hardware resource.
class Esp32s3PcmAudioPorts final {
   public:
    Esp32s3PcmAudioPorts(AudioBoardProfile profile, AudioPortOptions options = {});
    ~Esp32s3PcmAudioPorts();

    Esp32s3PcmAudioPorts(const Esp32s3PcmAudioPorts&) = delete;
    Esp32s3PcmAudioPorts& operator=(const Esp32s3PcmAudioPorts&) = delete;

    [[nodiscard]] voice::AudioInputPort& input();
    [[nodiscard]] voice::AudioOutputPort& output();
    [[nodiscard]] AudioPortStats stats() const;

   private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace voicelife::audio_esp
