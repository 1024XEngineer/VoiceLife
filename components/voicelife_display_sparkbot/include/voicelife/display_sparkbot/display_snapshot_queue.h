#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>

#include "voicelife/voice/display_snapshot.h"

namespace voicelife::display_sparkbot {

/**
 * @brief 有界 DisplaySnapshot 队列。
 *
 * 显示 Adapter 的 Render 只入队，专属显示任务消费；队列满时丢弃最旧
 * 快照（最新状态优先），保证显示阻塞不会反向阻塞语音/业务线程。
 * 旧 revision/generation 的丢弃由显示任务消费侧负责。
 */
class DisplaySnapshotQueue {
   public:
    /** @brief 构造函数。 @param capacity 队列容量（必须大于 0）。 */
    explicit DisplaySnapshotQueue(std::size_t capacity);

    /** @brief 有界入队：满时丢弃最旧快照。 @param snapshot 显示快照副本。 @return 始终 true（快照被接受）。 */
    bool Push(voicelife::voice::DisplaySnapshot snapshot);

    /** @brief 非阻塞取出最旧快照。 @param out 输出参数。 @return 队列非空时返回 true。 */
    bool Pop(voicelife::voice::DisplaySnapshot* out);

    /**
     * @brief 阻塞取出最旧快照（带超时）。
     * @param out 输出参数。
     * @param timeout_ms 超时毫秒数，0 表示无限等待。
     * @return 超时前取到快照返回 true。
     */
    bool WaitPop(voicelife::voice::DisplaySnapshot* out, int timeout_ms);

    /** @brief 当前元素数。 @return 队列大小。 */
    [[nodiscard]] std::size_t size() const;

    /** @brief 队列容量。 @return 容量。 */
    [[nodiscard]] std::size_t capacity() const;

    /** @brief 是否为空。 @return 空返回 true。 */
    [[nodiscard]] bool empty() const;

   private:
    mutable std::mutex mutex_;
    std::condition_variable not_empty_cv_;
    std::deque<voicelife::voice::DisplaySnapshot> queue_;
    std::size_t capacity_;
};

}  // namespace voicelife::display_sparkbot
