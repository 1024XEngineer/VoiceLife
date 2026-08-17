#include "voicelife/display_sparkbot/display_snapshot_queue.h"

namespace voicelife::display_sparkbot {

DisplaySnapshotQueue::DisplaySnapshotQueue(std::size_t capacity) : capacity_(capacity) {}

bool DisplaySnapshotQueue::Push(voicelife::voice::DisplaySnapshot snapshot) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.size() >= capacity_) {
        queue_.pop_front();  // 队列满：丢弃最旧，最新状态优先。
    }
    queue_.push_back(std::move(snapshot));
    not_empty_cv_.notify_one();
    return true;
}

bool DisplaySnapshotQueue::Pop(voicelife::voice::DisplaySnapshot* out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.empty()) {
        return false;
    }
    if (out != nullptr) {
        *out = std::move(queue_.front());
    }
    queue_.pop_front();
    return true;
}

bool DisplaySnapshotQueue::WaitPop(voicelife::voice::DisplaySnapshot* out, int timeout_ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    const auto wait_ready = [this] { return !queue_.empty(); };
    if (timeout_ms <= 0) {
        not_empty_cv_.wait(lock, wait_ready);
    } else {
        if (!not_empty_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), wait_ready)) {
            return false;
        }
    }
    if (out != nullptr) {
        *out = std::move(queue_.front());
    }
    queue_.pop_front();
    return true;
}

std::size_t DisplaySnapshotQueue::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}

std::size_t DisplaySnapshotQueue::capacity() const { return capacity_; }

bool DisplaySnapshotQueue::empty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.empty();
}

}  // namespace voicelife::display_sparkbot
