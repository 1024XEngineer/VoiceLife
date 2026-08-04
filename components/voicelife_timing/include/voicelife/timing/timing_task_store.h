#pragma once

#include <cstdint>
#include <string>

#include "voicelife/contracts/status.h"
#include "voicelife/timing/timing_task_types.h"

namespace voicelife::timing {

/// 持久化定时任务的边界。
class TimingTaskStorePort {
   public:
    /** @brief 允许通过接口类型释放存储适配器。 */
    virtual ~TimingTaskStorePort() = default;
    /**
     * @brief 保存一条定时任务，并在持久化完成后返回。
     * @param task 要保存的任务。
     * @return 保存结果。
     */
    virtual Status SaveTask(const TimingTask& task) = 0;
};

/// 提供可替换的当前时间来源。
class TimingClockPort {
   public:
    /** @brief 允许通过接口类型释放时钟适配器。 */
    virtual ~TimingClockPort() = default;
    /** @brief 返回当前 Unix 秒级时间戳。 @return 当前时间。 */
    virtual int64_t Now() const = 0;
};

/// 提供可替换的定时任务标识生成器。
class TimingIdGeneratorPort {
   public:
    /** @brief 允许通过接口类型释放标识生成器。 */
    virtual ~TimingIdGeneratorPort() = default;
    /** @brief 返回下一条定时任务标识。 @return 新任务标识。 */
    virtual std::string NextTaskId() = 0;
};

}  // namespace voicelife::timing
