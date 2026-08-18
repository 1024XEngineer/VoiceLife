#pragma once

#include <atomic>
#include <cstdint>

namespace voicelife::runtime {

/**
 * 绑定轮询任务的代次所有权。
 * 一个仍在退出中的旧任务可以接管新会话，且只能释放它仍持有的代次。
 */
class BindingPollingLease {
   public:
    /** @brief 获取或移交轮询所有权。 @return true 时调用方须创建新任务。 */
    bool Acquire(uint64_t generation) {
        uint64_t observed = generation_.load(std::memory_order_acquire);
        while (true) {
            if (observed == generation) return false;
            if (generation_.compare_exchange_weak(observed, generation, std::memory_order_acq_rel,
                                                  std::memory_order_acquire)) {
                return observed == 0;
            }
        }
    }

    /** @brief 仅在调用方仍持有该代次时释放轮询所有权。 */
    bool Release(uint64_t generation) {
        uint64_t expected = generation;
        return generation_.compare_exchange_strong(expected, 0, std::memory_order_acq_rel, std::memory_order_acquire);
    }

    /** @brief 返回当前轮询任务负责的代次。 */
    [[nodiscard]] uint64_t generation() const { return generation_.load(std::memory_order_acquire); }

   private:
    std::atomic<uint64_t> generation_{0};
};

}  // namespace voicelife::runtime
