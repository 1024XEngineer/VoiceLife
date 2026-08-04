#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include "voicelife/contracts/status.h"
#include "voicelife/voice/voice_types.h"

namespace voicelife::audio_esp {

// Converts fixed hardware periods into the frame duration negotiated by the
// Provider. The hardware period is deliberately not part of AudioFormat: it
// is a board scheduling detail, while AudioFormat is a transport contract.
class PcmFrameAssembler final {
   public:
    using Sink = std::function<Status(voice::AudioFrame)>;

    PcmFrameAssembler(voice::AudioFormat frame_format, uint16_t hardware_period_ms);

    [[nodiscard]] Status Validate() const;
    [[nodiscard]] const voice::AudioFormat& frame_format() const { return frame_format_; }
    [[nodiscard]] std::size_t frame_samples() const { return frame_samples_; }
    [[nodiscard]] std::size_t pending_samples() const { return pending_samples_.size(); }

    // Accepts logical S16 samples. A sink is called only for complete frames;
    // if it fails, the frame has already been removed from the assembler and
    // the caller must account for the drop in its own bounded queue metrics.
    Status Push(const int16_t* samples, std::size_t sample_count, const Sink& sink);
    void Reset();

   private:
    voice::AudioFormat frame_format_;
    uint16_t hardware_period_ms_ = 0;
    std::size_t frame_samples_ = 0;
    std::vector<int16_t> pending_samples_;
};

}  // namespace voicelife::audio_esp
