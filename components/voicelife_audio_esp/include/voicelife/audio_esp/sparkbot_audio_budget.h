#pragma once

#include <cstddef>
#include <cstdint>

namespace voicelife::audio_esp {

// The decode pool must retain every frame accepted by the playback queue,
// plus the frame currently writing to I2S and the decode candidate being
// checked for admission.
inline constexpr uint32_t kSparkBotOpusFrameDurationMs = 20;
inline constexpr std::size_t kSparkBotPlaybackQueueDepth = 96;
inline constexpr uint32_t kSparkBotPlaybackLatencyBudgetMs = 1920;
inline constexpr std::size_t kSparkBotPlaybackWriterFrames = 1;
inline constexpr std::size_t kSparkBotOpusDecodeCandidateFrames = 1;
inline constexpr std::size_t kSparkBotOpusDecodePoolSlots =
    kSparkBotPlaybackQueueDepth + kSparkBotPlaybackWriterFrames + kSparkBotOpusDecodeCandidateFrames;

/**
 * @brief 确保播放队列深度与声明的延迟预算一致。
 * @param kSparkBotOpusFrameDurationMs 单个 Opus 帧的时长常量。
 */
static_assert(kSparkBotPlaybackQueueDepth * kSparkBotOpusFrameDurationMs == kSparkBotPlaybackLatencyBudgetMs);
/** @brief 确保解码池覆盖队列、写入帧和候选帧。 */
static_assert(kSparkBotOpusDecodePoolSlots >= kSparkBotPlaybackQueueDepth + 2);

}  // namespace voicelife::audio_esp
