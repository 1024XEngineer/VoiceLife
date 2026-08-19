#pragma once

#include <cstdint>
#include <mutex>
#include <utility>

namespace voicelife::linx_esp {

/**
 * @brief 将 TX generation 检查与实际写操作原子化。
 *
 * 发送者必须通过 SendIfCurrent() 执行 WebSocket 写；SetGeneration() 返回后，
 * 已经切换前旧代次的 item 不会再开始写入。正在执行的同步写会先完成，随后
 * 才允许 generation 前进。
 */
class LinxTxGenerationGate final {
   public:
    /** @brief 更新允许发送的 generation。 @param generation 新 generation。 */
    void SetGeneration(uint64_t generation);

    /**
     * @brief 当前 generation 匹配时，在同一临界区执行一次发送。
     * @tparam Sender 无参、同步完成的发送函数类型。
     * @param item_generation 队列项创建时绑定的 generation。
     * @param sender 实际写入函数。
     * @return 是否已执行 sender。
     */
    template <typename Sender>
    bool SendIfCurrent(uint64_t item_generation, Sender&& sender);

   private:
    std::mutex mutex_;
    uint64_t generation_ = 0;
};

inline void LinxTxGenerationGate::SetGeneration(uint64_t generation) {
    std::lock_guard<std::mutex> lock(mutex_);
    generation_ = generation;
}

template <typename Sender>
bool LinxTxGenerationGate::SendIfCurrent(uint64_t item_generation, Sender&& sender) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (item_generation != generation_) return false;
    std::forward<Sender>(sender)();
    return true;
}

}  // namespace voicelife::linx_esp
