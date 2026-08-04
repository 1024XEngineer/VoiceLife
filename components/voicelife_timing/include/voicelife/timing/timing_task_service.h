#pragma once

#include "voicelife/contracts/status.h"
#include "voicelife/timing/timing_task_contracts.h"
#include "voicelife/timing/timing_task_store.h"

namespace voicelife::timing {

/// 定义定时任务模块对调用方公开的用例边界。
class TimingTaskService {
   public:
    /** @brief 允许通过接口类型释放服务实现。 */
    virtual ~TimingTaskService() = default;
    /**
     * @brief 注册一条一次性或周期定时任务。
     * @param command 要注册的日程定时信息。
     * @return 注册结果或校验、持久化错误。
     */
    virtual Result<RegisterTimerTaskResult> RegisterTimerTask(const RegisterTimerTaskCommand& command) = 0;
    /**
     * @brief 按指定范围修改定时任务。
     * @param command 修改内容与作用范围。
     * @return 修改结果或领域错误。
     */
    virtual Result<UpdateTimerTaskResult> UpdateTimerTask(const UpdateTimerTaskCommand& command) = 0;
    /**
     * @brief 按指定范围取消定时任务。
     * @param command 取消目标与作用范围。
     * @return 取消结果或领域错误。
     */
    virtual Result<CancelTimerTaskResult> CancelTimerTask(const CancelTimerTaskCommand& command) = 0;
    /**
     * @brief 创建或更新任务的提醒规则。
     * @param command 任务标识和规则列表。
     * @return 当前完整规则列表或领域错误。
     */
    virtual Result<UpsertReminderRulesResult> UpsertReminderRules(const UpsertReminderRulesCommand& command) = 0;
    /**
     * @brief 关闭一条提醒规则。
     * @param command 要关闭的规则标识。
     * @return 关闭结果或领域错误。
     */
    virtual Result<DeleteReminderRuleResult> DeleteReminderRule(const DeleteReminderRuleCommand& command) = 0;
    /**
     * @brief 查询时间范围内的用户可见安排。
     * @param query 时间范围、过滤和分页条件。
     * @return 分页后的日历视图或查询错误。
     */
    virtual Result<CalendarView> ListCalendarView(const CalendarViewQuery& query) = 0;
    /**
     * @brief 查询符合条件的提醒触发。
     * @param query 过滤、时间范围和分页条件。
     * @return 分页后的提醒触发或查询错误。
     */
    virtual Result<ReminderTriggerPage> ListReminderTriggers(const ReminderTriggerQuery& query) = 0;
    /**
     * @brief 推迟一次强提醒。
     * @param command 提醒标识和推迟分钟数。
     * @return 更新后的提醒触发或领域错误。
     */
    virtual Result<ReminderTrigger> SnoozeReminderTrigger(const SnoozeReminderTriggerCommand& command) = 0;
    /**
     * @brief 关闭一次强提醒。
     * @param command 要关闭的提醒触发标识。
     * @return 更新后的提醒触发或领域错误。
     */
    virtual Result<ReminderTrigger> DismissReminderTrigger(const DismissReminderTriggerCommand& command) = 0;
};

/// 使用领域策略和注入端口实现定时任务用例。
class DefaultTimingTaskService final : public TimingTaskService {
   public:
    /**
     * @brief 使用指定端口创建服务。
     * @param store 任务存储。
     * @param clock 当前时间来源。
     * @param ids 任务标识生成器。
     */
    DefaultTimingTaskService(TimingTaskStorePort& store, TimingClockPort& clock, TimingIdGeneratorPort& ids)
        : store_(store), clock_(clock), ids_(ids) {}

    /**
     * @brief 注册并持久化一条一次性或周期定时任务。
     * @param command 要注册的日程定时信息。
     * @return 注册结果或校验、持久化错误。
     */
    Result<RegisterTimerTaskResult> RegisterTimerTask(const RegisterTimerTaskCommand& command) override;
    /**
     * @brief 按指定范围修改定时任务。
     * @param command 修改内容与作用范围。
     * @return 修改结果或领域错误。
     */
    Result<UpdateTimerTaskResult> UpdateTimerTask(const UpdateTimerTaskCommand& command) override;
    /**
     * @brief 按指定范围取消定时任务。
     * @param command 取消目标与作用范围。
     * @return 取消结果或领域错误。
     */
    Result<CancelTimerTaskResult> CancelTimerTask(const CancelTimerTaskCommand& command) override;
    /**
     * @brief 创建或更新任务的提醒规则。
     * @param command 任务标识和规则列表。
     * @return 当前完整规则列表或领域错误。
     */
    Result<UpsertReminderRulesResult> UpsertReminderRules(const UpsertReminderRulesCommand& command) override;
    /**
     * @brief 关闭一条提醒规则。
     * @param command 要关闭的规则标识。
     * @return 关闭结果或领域错误。
     */
    Result<DeleteReminderRuleResult> DeleteReminderRule(const DeleteReminderRuleCommand& command) override;
    /**
     * @brief 查询时间范围内的用户可见安排。
     * @param query 时间范围、过滤和分页条件。
     * @return 分页后的日历视图或查询错误。
     */
    Result<CalendarView> ListCalendarView(const CalendarViewQuery& query) override;
    /**
     * @brief 查询符合条件的提醒触发。
     * @param query 过滤、时间范围和分页条件。
     * @return 分页后的提醒触发或查询错误。
     */
    Result<ReminderTriggerPage> ListReminderTriggers(const ReminderTriggerQuery& query) override;
    /**
     * @brief 推迟一次强提醒。
     * @param command 提醒标识和推迟分钟数。
     * @return 更新后的提醒触发或领域错误。
     */
    Result<ReminderTrigger> SnoozeReminderTrigger(const SnoozeReminderTriggerCommand& command) override;
    /**
     * @brief 关闭一次强提醒。
     * @param command 要关闭的提醒触发标识。
     * @return 更新后的提醒触发或领域错误。
     */
    Result<ReminderTrigger> DismissReminderTrigger(const DismissReminderTriggerCommand& command) override;

   private:
    TimingTaskStorePort& store_;
    TimingClockPort& clock_;
    TimingIdGeneratorPort& ids_;
};

}  // namespace voicelife::timing
