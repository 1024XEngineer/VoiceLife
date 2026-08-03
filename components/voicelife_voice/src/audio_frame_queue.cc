#include "voicelife/voice/audio_frame_queue.h"

#include <utility>

namespace voicelife::voice {

BoundedAudioFrameQueue::BoundedAudioFrameQueue(AudioQueuePolicy policy) : policy_(policy) {}

Status BoundedAudioFrameQueue::Push(AudioFrame frame) {
    if (!policy_.valid()) {
        return Status::Error(ErrorCode::kInvalidArgument, "音频队列容量必须大于零");
    }
    if (!frame.format.valid() || frame.payload.empty()) {
        return Status::Error(ErrorCode::kInvalidArgument, "音频帧格式或负载无效");
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (frame.generation != generation_) {
        ++stats_.rejected_stale_generation;
        return Status::Error(ErrorCode::kConflict, "音频帧 generation 已过期");
    }
    if (frames_.size() >= policy_.capacity_frames) {
        if (policy_.full_policy == AudioQueueFullPolicy::kRejectNewest) {
            ++stats_.rejected_newest;
            return Status::Error(ErrorCode::kUnavailable, "音频队列已满");
        }
        frames_.pop_front();
        ++stats_.dropped_oldest;
    }
    frames_.push_back(std::move(frame));
    if (frames_.size() > stats_.high_watermark) {
        stats_.high_watermark = frames_.size();
    }
    return Status::Ok();
}

Result<AudioFrame> BoundedAudioFrameQueue::Pop() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (frames_.empty()) {
        return Result<AudioFrame>::Failure(ErrorCode::kNotFound, "音频队列为空");
    }
    AudioFrame frame = std::move(frames_.front());
    frames_.pop_front();
    return Result<AudioFrame>::Success(std::move(frame));
}

Status BoundedAudioFrameQueue::SetGeneration(std::uint64_t generation) {
    if (!policy_.valid()) {
        return Status::Error(ErrorCode::kInvalidArgument, "音频队列容量必须大于零");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (generation_ != generation) {
        frames_.clear();
        generation_ = generation;
    }
    return Status::Ok();
}

void BoundedAudioFrameQueue::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    frames_.clear();
}

std::size_t BoundedAudioFrameQueue::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return frames_.size();
}

bool BoundedAudioFrameQueue::empty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return frames_.empty();
}

std::uint64_t BoundedAudioFrameQueue::generation() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return generation_;
}

AudioQueueStats BoundedAudioFrameQueue::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

}  // namespace voicelife::voice
