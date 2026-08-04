#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "voicelife/contracts/status.h"
#include "voicelife/timing/timing_task_types.h"

namespace voicelife::timing {

/// 持久化定时任务的边界。
class TimingTaskStorePort {
   public:
    /** @brief 允许通过接口类型释放存储适配器。 */
    virtual ~TimingTaskStorePort() = default;
    /**
     * @brief 原子保存任务及其初始提醒规则。
     * @param task 要注册的任务。
     * @param rules 与任务同时提交的提醒规则。
     * @return 全部提交成功或完全不写入的结果；重复标识返回冲突。
     */
    virtual Status RegisterTaskWithRules(const TimingTask& task, const std::vector<ReminderRule>& rules) = 0;
    /**
     * @brief 按标识查询任务。
     * @param task_id 定时任务标识。
     * @return 找到的任务或不存在错误。
     */
    virtual Result<TimingTask> FindTask(const TimingTaskId& task_id) = 0;
    /**
     * @brief 查询任务当前的提醒规则。
     * @param task_id 定时任务标识。
     * @return 规则列表或存储错误。
     */
    virtual Result<std::vector<ReminderRule>> ListRules(const TimingTaskId& task_id) = 0;
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
    /** @brief 返回下一条提醒规则标识。 @return 新规则标识。 */
    virtual std::string NextReminderRuleId() = 0;
};

}  // namespace voicelife::timing
