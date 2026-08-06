#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "voicelife/contracts/status.h"
#include "voicelife/timing/timing_task_types.h"

namespace voicelife::timing {

/// 表示一次定时任务修改需要原子提交的任务字段和 occurrence 变更。
struct TimingTaskUpdateWrite {
    TimingTask task{};
    std::vector<TimerInstance> upsert_instances{};
};

/// 持久化定时任务的边界。
class TimingTaskStorePort {
   public:
    /** @brief 允许通过接口类型释放存储适配器。 */
    virtual ~TimingTaskStorePort() = default;
    /**
     * @brief 原子保存任务及其初始提醒规则。
     * @param task 要注册的任务。
     * @param rules 与任务同时提交的提醒规则。
     * @return 全部提交成功或完全不写入的结果；重复 request_id、task_id 或 schedule_id 返回冲突。
     */
    virtual Status RegisterTaskWithRules(const TimingTask& task, const std::vector<ReminderRule>& rules) = 0;
    /**
     * @brief 按幂等请求标识查询已注册任务。
     * @param request_id 注册命令的幂等标识。
     * @return 已保存的任务、不存在结果或存储错误。
     */
    virtual Result<TimingTask> FindTaskByRequestId(const std::string& request_id) = 0;
    /**
     * @brief 原子提交任务字段和 occurrence 覆盖或状态变更。
     * @param update 要保存的任务和实例变更；更新范围内的旧实例可标记为 skipped。
     * @return 全部提交成功或完全不写入的结果；目标不存在、实例归属错误或存储失败返回领域错误。
     */
    virtual Status UpdateTaskWithInstances(const TimingTaskUpdateWrite& update) = 0;
    /**
     * @brief 按标识查询任务。
     * @param task_id 定时任务标识。
     * @return 找到的任务或不存在错误。
     */
    virtual Result<TimingTask> FindTask(const TimingTaskId& task_id) = 0;
    /**
     * @brief 查询全部定时任务。
     * @return 任务列表或存储错误；日历服务负责按日程和生命周期过滤。
     */
    virtual Result<std::vector<TimingTask>> ListTasks() = 0;
    /**
     * @brief 查询任务当前的提醒规则。
     * @param task_id 定时任务标识。
     * @return 规则列表或存储错误。
     */
    virtual Result<std::vector<ReminderRule>> ListRules(const TimingTaskId& task_id) = 0;
    /**
     * @brief 原子关闭提醒规则并取消尚未发生的触发。
     * @param reminder_rule_id 要关闭的规则标识。
     * @param now 当前 Unix 秒级时间戳；不早于此时间的 pending trigger 视为未来触发。
     * @return 受影响的未来 trigger 数量；规则不存在返回 not found，已关闭或关联错误返回冲突。
     */
    virtual Result<int> DisableReminderRule(const std::string& reminder_rule_id, int64_t now) = 0;
    /**
     * @brief 原子创建或更新同一任务的一组提醒规则。
     * @param task_id 规则所属的定时任务标识。
     * @param rules 要创建或更新的规则；该操作不修改已物化的提醒触发。
     * @return 全部规则提交成功或完全不写入的结果；实现必须在同一原子边界内保证每个任务至多一条 active 准点强提醒规则。
     */
    virtual Status UpsertRules(const TimingTaskId& task_id, const std::vector<ReminderRule>& rules) = 0;
    /**
     * @brief 查询任务已物化的 occurrence 实例。
     * @param task_id 定时任务标识。
     * @return 包含软删除记录的实例列表或存储错误；日历服务负责决定它们是否用户可见。
     */
    virtual Result<std::vector<TimerInstance>> ListInstances(const TimingTaskId& task_id) = 0;
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
