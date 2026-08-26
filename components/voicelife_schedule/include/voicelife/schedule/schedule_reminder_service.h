#pragma once

#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "voicelife/contracts/status.h"
#include "voicelife/schedule/schedule_reminder_task_repository.h"
#include "voicelife/schedule/schedule_repository.h"
#include "voicelife/schedule/schedule_rule_service.h"
#include "voicelife/schedule/schedule_service.h"

namespace voicelife::timing {
/** @brief 提供一次性定时任务调度能力的服务。 */
class TimingTaskService;
}  // namespace voicelife::timing

namespace voicelife::schedule {

/** @brief 提供提醒语音播报能力的接口。 */
class ScheduleReminderSpeechPort {
   public:
    /** @brief 析构提醒语音播报端口。 */
    virtual ~ScheduleReminderSpeechPort() = default;
    /** @brief 播报提醒文本。
     * @param text 待播报的提醒内容。
     * @return 播报操作状态。
     */
    virtual Status SpeakScheduleReminder(std::string_view text) = 0;
};

/** @brief 可选的提醒通知出口，由 IM 适配器在组件外实现。 */
class ScheduleReminderNotificationPort {
   public:
    /** @brief 析构提醒通知端口。 */
    virtual ~ScheduleReminderNotificationPort() = default;
    /** @brief 发送日程提醒通知。
     * @param schedule 触发提醒的日程。
     * @param task 当前提醒任务记录。
     * @return 发送操作状态。
     */
    virtual Status SendScheduleReminder(const Schedule& schedule, const ScheduleReminderTask& task) = 0;
};

/** @brief 设备本地提醒动作命令；协议 Adapter 负责映射外部字段。 */
struct ReminderActionCommand {
    std::string operation_id;
    std::string reminder_trigger_id;
    ScheduleReminderActionKind action = ScheduleReminderActionKind::kAcknowledge;
    std::optional<int> snooze_minutes;
};

/** @brief 提醒动作的已提交结果。 */
struct ReminderActionResult {
    int affected_count = 0;
    std::vector<std::string> events;
    std::string operation_id;
    std::string reminder_trigger_id;
    ScheduleReminderActionKind action = ScheduleReminderActionKind::kAcknowledge;
    DateTime occurred_at;
    std::optional<DateTime> next_trigger_at;
    bool replayed = false;
};

/** @brief 协调持久化提醒记录、一次性 Timing 任务、语音和通知。 */
class ScheduleReminderService final {
   public:
    using NowProvider = std::function<DateTime()>;

    /** @brief 构造提醒服务。
     * @param repository 日程仓储。
     * @param reminder_repository 提醒任务仓储。
     * @param schedule_service 日程服务。
     * @param rule_service 规则服务。
     * @param timing_service 定时任务服务。
     * @param speech 语音播报端口。
     * @param notification 通知端口。
     * @param now_provider 当前时间提供器。
     */
    ScheduleReminderService(ScheduleRepository& repository, ScheduleReminderTaskRepository& reminder_repository,
                            ScheduleService& schedule_service, ScheduleRuleService& rule_service,
                            timing::TimingTaskService& timing_service, ScheduleReminderSpeechPort& speech,
                            ScheduleReminderNotificationPort* notification = nullptr, NowProvider now_provider = {});

    /** @brief 启动提醒服务。
     * @return 启动操作状态。
     */
    Status Start();
    /** @brief 停止提醒服务。 */
    void Stop();
    /** @brief 同步指定日程的提醒。
     * @param schedule_id 日程标识。
     * @return 同步操作状态。
     */
    Status SynchronizeSchedule(ScheduleId schedule_id);
    /** @brief 取消指定日程的提醒。
     * @param schedule_id 日程标识。
     * @return 取消操作状态。
     */
    Status CancelScheduleReminder(ScheduleId schedule_id);
    /** @brief 暂停规则下的提醒。
     * @param rule_id 规则标识。
     * @return 暂停操作状态。
     */
    Status SuspendRuleReminders(ScheduleRuleId rule_id);
    /** @brief 同步指定规则的提醒。
     * @param rule_id 规则标识。
     * @return 同步操作状态。
     */
    Status SynchronizeRule(ScheduleRuleId rule_id);
    /** @brief 确认最近触发的提醒。
     * @return 动作结果或错误状态。
     */
    Result<ReminderActionResult> AcknowledgeRecentReminders();
    /** @brief 延后最近触发的提醒。
     * @return 动作结果或错误状态。
     */
    Result<ReminderActionResult> SnoozeRecentReminders();
    /** @brief 按精确触发标识幂等执行提醒动作。
     * @param command 本地动作命令。
     * @return 首次提交或持久化重放的动作结果。
     */
    Result<ReminderActionResult> ExecuteReminderAction(const ReminderActionCommand& command);
    /**
     * @brief 对最近触发的每个提醒逐条执行动作，返回可上报的持久化结果。
     * @param action acknowledge 或 snooze。
     * @return 每个提醒一条结果；没有可操作提醒时返回失败。
     */
    Result<std::vector<ReminderActionResult>> ExecuteRecentReminderActions(ScheduleReminderActionKind action);
    /**
     * @brief 读取已持久化的语音动作事实，供网络恢复后补报。
     * @return 已保存的语音动作结果，或仓储错误。
     */
    Result<std::vector<ReminderActionResult>> ListPersistedVoiceActionResults() const;

   private:
    /// @brief 规则提醒生成的重试状态。
    struct RetryState {
        std::string task_id;
        int failure_count = 0;
    };

    DateTime Now() const;
    int64_t AllocateChainId();
    std::string AllocateTaskId(std::string_view prefix);
    Status RegisterReminder(ScheduleId schedule_id, int64_t chain_id, int attempt, DateTime trigger_at);
    Status RegisterPersistedTask(const ScheduleReminderTask& task);
    Status CancelPendingTasks(ScheduleId schedule_id, std::optional<int64_t> except_task_id = std::nullopt);
    void HandleReminder(int64_t reminder_task_id, std::string_view timing_task_id);
    void GenerateNextInstance(ScheduleRuleId rule_id, int prior_failure_count);
    Status ScheduleGenerationRetry(ScheduleRuleId rule_id, int failure_count);
    bool IsRunning() const;

    ScheduleRepository& repository_;
    ScheduleReminderTaskRepository& reminder_repository_;
    ScheduleService& schedule_service_;
    ScheduleRuleService& rule_service_;
    timing::TimingTaskService* timing_service_;
    ScheduleReminderSpeechPort& speech_;
    ScheduleReminderNotificationPort* notification_;
    NowProvider now_provider_;
    mutable std::mutex mutex_;
    std::mutex action_mutex_;
    bool running_ = false;
    int64_t sequence_ = 0;
    int64_t chain_sequence_ = 0;
    std::unordered_map<ScheduleRuleId, RetryState> generation_retries_;
};

}  // namespace voicelife::schedule
