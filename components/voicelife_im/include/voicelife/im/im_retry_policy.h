#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include "voicelife/im/im_transport.h"

namespace voicelife::im {

/// IM 网络操作的有限指数退避参数。
struct ImRetryOptions {
    /// 单轮最多返回的重试次数。
    std::size_t maximum_attempts = 4;
    /// 第一次可重试失败后的等待时间。
    uint32_t initial_delay_ms = 1000;
    /// 指数增长后的等待时间上限。
    uint32_t maximum_delay_ms = 30000;
};

/**
 * @brief 对 IM Transport 结果作有限、无紧密循环的重试决策。
 *
 * 网络失败、408、429 与 5xx 可以退避重试；401/403、其他 4xx 和
 * 本地配置错误均立即停止。此类型只计算延迟，不创建任务也不执行等待。
 */
class ImRetryPolicy {
   public:
    /** @brief 创建退避策略。 @param options 有限次数与延迟边界。 */
    explicit ImRetryPolicy(ImRetryOptions options = {});

    /**
     * @brief 返回下一次重试前的等待时间。
     * @param response 最近一次 Transport 结果。
     * @return 可重试且未耗尽时为毫秒数，否则为空。
     */
    std::optional<uint32_t> NextDelay(const ImHttpResponse& response);

    /** @brief 开始新一轮操作并清除已使用的重试次数。 */
    void Reset();

    /** @brief 返回本轮已批准的重试次数。 @return 当前计数。 */
    [[nodiscard]] std::size_t attempts() const { return attempts_; }

   private:
    ImRetryOptions options_;
    std::size_t attempts_ = 0;
};

}  // namespace voicelife::im
