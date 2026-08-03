#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>

#include "voicelife/contracts/status.h"
#include "voicelife/voice/voice_types.h"

namespace voicelife::voice {

enum class AudioQueueFullPolicy {
    // Realtime capture cannot wait for a slow network consumer. Preserve the
    // newest audio and report the discarded history through statistics.
    kDropOldest,
    // Playback must not silently discard already queued speech. Reject the
    // incoming frame so the adapter can surface an underrun/overflow event.
    kRejectNewest,
};

struct AudioQueuePolicy {
    std::size_t capacity_frames = 0;
    AudioQueueFullPolicy full_policy = AudioQueueFullPolicy::kDropOldest;

    [[nodiscard]] bool valid() const { return capacity_frames > 0; }
};

struct AudioQueueStats {
    std::size_t high_watermark = 0;
    std::uint64_t dropped_oldest = 0;
    std::uint64_t rejected_newest = 0;
    std::uint64_t rejected_stale_generation = 0;
};

// A bounded, generation-aware queue shared by host fakes and board adapters.
// It owns frame payloads and never blocks a producer while applying a full
// queue policy. A generation change is a hard playback/capture boundary.
class BoundedAudioFrameQueue final {
   public:
    explicit BoundedAudioFrameQueue(AudioQueuePolicy policy);

    [[nodiscard]] Status Push(AudioFrame frame);
    [[nodiscard]] Result<AudioFrame> Pop();

    // Clears queued frames and makes subsequent pushes use the new generation.
    // Calling this for the current generation is intentionally idempotent.
    [[nodiscard]] Status SetGeneration(std::uint64_t generation);
    void Clear();

    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] bool empty() const;
    [[nodiscard]] std::uint64_t generation() const;
    [[nodiscard]] AudioQueueStats stats() const;

   private:
    AudioQueuePolicy policy_;
    mutable std::mutex mutex_;
    std::deque<AudioFrame> frames_;
    std::uint64_t generation_ = 0;
    AudioQueueStats stats_;
};

}  // namespace voicelife::voice
