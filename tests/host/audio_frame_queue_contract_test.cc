#include "support/test_support.h"
#include "voicelife/voice/audio_frame_queue.h"

using voicelife::ErrorCode;
using voicelife::test::Check;

namespace {

voicelife::voice::AudioFrame Frame(std::uint64_t generation, std::uint64_t sequence) {
    voicelife::voice::AudioFrame frame;
    frame.generation = generation;
    frame.sequence = sequence;
    frame.payload = {static_cast<std::uint8_t>(sequence + 1)};
    return frame;
}

}  // namespace

int main() {
    using voicelife::voice::AudioQueueFullPolicy;
    using voicelife::voice::AudioQueuePolicy;
    using voicelife::voice::BoundedAudioFrameQueue;

    BoundedAudioFrameQueue invalid({});
    Check(invalid.Push(Frame(0, 0)).code == ErrorCode::kInvalidArgument, "容量为零的队列必须拒绝写入");

    BoundedAudioFrameQueue capture({.capacity_frames = 2, .full_policy = AudioQueueFullPolicy::kDropOldest});
    Check(capture.SetGeneration(7).ok(), "队列应能建立初始 generation");
    Check(capture.Push(Frame(7, 0)).ok() && capture.Push(Frame(7, 1)).ok(), "当前 generation 的帧应进入采集队列");
    Check(capture.Push(Frame(7, 2)).ok() && capture.size() == 2, "采集队列满载时应保留固定容量");
    auto first = capture.Pop();
    Check(first.ok() && first.value->sequence == 1, "drop-oldest 必须丢弃最早帧而保留最新音频");
    auto capture_stats = capture.stats();
    Check(capture_stats.dropped_oldest == 1 && capture_stats.high_watermark == 2, "采集队列必须暴露丢帧和水位统计");
    Check(capture.Push(Frame(6, 3)).code == ErrorCode::kConflict, "旧 generation 不能重新进入队列");
    Check(capture.stats().rejected_stale_generation == 1, "旧 generation 拒绝必须计数");

    BoundedAudioFrameQueue playback({.capacity_frames = 1, .full_policy = AudioQueueFullPolicy::kRejectNewest});
    Check(playback.SetGeneration(3).ok() && playback.Push(Frame(3, 0)).ok(), "播放队列应接受第一帧");
    Check(playback.Push(Frame(3, 1)).code == ErrorCode::kUnavailable, "播放队列满载时不得静默丢掉已排队语音");
    Check(playback.stats().rejected_newest == 1, "播放队列拒绝新帧必须计数");
    Check(playback.SetGeneration(4).ok() && playback.empty(), "切换 generation 必须清理旧播放帧");
    Check(playback.Push(Frame(4, 0)).ok(), "新 generation 应可重新入队");
    Check(playback.Pop().value->generation == 4, "队列应只返回当前 generation 的帧");

    return 0;
}
