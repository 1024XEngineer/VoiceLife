#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>

#include "voicelife/contracts/status.h"
#include "voicelife/voice/voice_types.h"

namespace voicelife::voice {

/** 定义有界音频队列满载时的丢弃策略。 */
enum class AudioQueueFullPolicy {
    // Realtime capture cannot wait for a slow network consumer. Preserve the
    // newest audio and report the discarded history through statistics.
    kDropOldest,
    // Playback must not silently discard already queued speech. Reject the
    // incoming frame so the adapter can surface an underrun/overflow event.
    kRejectNewest,
};

/** 描述有界音频队列的容量和满载策略。 */
struct AudioQueuePolicy {
    std::size_t capacity_frames = 0;
    AudioQueueFullPolicy full_policy = AudioQueueFullPolicy::kDropOldest;

    /**
     * @brief 校验队列容量是否可用。
     * @return 容量大于零时返回 true。
     */
    [[nodiscard]] bool valid() const { return capacity_frames > 0; }
};

/** 汇总队列丢弃、拒绝和高水位统计。 */
struct AudioQueueStats {
    std::size_t high_watermark = 0;
    std::uint64_t dropped_oldest = 0;
    std::uint64_t rejected_newest = 0;
    std::uint64_t rejected_stale_generation = 0;
};

/** 有界、按会话代次隔离的音频队列，供主机替身和板级适配器共用。 */
class BoundedAudioFrameQueue final {
   public:
    /**
     * @brief 按策略创建有界音频队列。
     * @param policy 队列容量和满载策略。
     */
    explicit BoundedAudioFrameQueue(AudioQueuePolicy policy);

    /**
     * @brief 推入一帧音频并应用满载策略。
     * @param frame 待推入的音频帧。
     * @return 推入结果。
     */
    [[nodiscard]] Status Push(AudioFrame frame);
    /**
     * @brief 取出最早的一帧音频。
     * @return 队头音频帧或空队列错误。
     */
    [[nodiscard]] Result<AudioFrame> Pop();

    // Clears queued frames and makes subsequent pushes use the new generation.
    // Calling this for the current generation is intentionally idempotent.
    /**
     * @brief 切换会话代次并清空旧代次数据。
     * @param generation 新的会话代次。
     * @return 切换结果。
     */
    [[nodiscard]] Status SetGeneration(std::uint64_t generation);
    /** @brief 清空队列内容但保留当前代次和统计。 */
    void Clear();

    /** @brief 返回当前队列帧数。 @return 队列中的帧数。 */
    [[nodiscard]] std::size_t size() const;
    /** @brief 判断队列是否为空。 @return 队列为空时返回 true。 */
    [[nodiscard]] bool empty() const;
    /** @brief 返回队列当前会话代次。 @return 当前代次。 */
    [[nodiscard]] std::uint64_t generation() const;
    /** @brief 读取队列统计快照。 @return 当前统计数据。 */
    [[nodiscard]] AudioQueueStats stats() const;

   private:
    AudioQueuePolicy policy_;
    mutable std::mutex mutex_;
    std::deque<AudioFrame> frames_;
    std::uint64_t generation_ = 0;
    AudioQueueStats stats_;
};

}  // namespace voicelife::voice
