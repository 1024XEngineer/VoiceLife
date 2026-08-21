#pragma once

#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "voicelife/contracts/status.h"
#include "voicelife/schedule/schedule_reminder_task_repository.h"
#include "voicelife/schedule/schedule_repository.h"
#include "voicelife/schedule/schedule_rule_service.h"
#include "voicelife/schedule/schedule_service.h"

namespace voicelife::timing {
class TimingTaskService;
}

namespace voicelife::schedule {

/// @brief 提供提醒语音播报能力的接口。
class ScheduleReminderSpeechPort {
   public:
    /// @brief 播报提醒文本。
    /// @param text 待播报的提醒内容。
    /// @return 播报操作状态。
    virtual ~ScheduleReminderSpeechPort() = default;
    virtual Status SpeakScheduleReminder(std::string_view text) = 0;
};

/// @brief 可选的提醒通知出口，由 IM 适配器在组件外实现。
class ScheduleReminderNotificationPort {
   public:
    /// @brief 发送日程提醒通知。
    /// @param schedule 触发提醒的日程。
    /// @param task 当前提醒任务记录。
    /// @return 发送操作状态。
    virtual ~ScheduleReminderNotificationPort() = default;
    virtual Status SendScheduleReminder(const Schedule& schedule, const ScheduleReminderTask& task) = 0;
};

/// @brief 提醒动作的执行结果。
/// @brief 提醒动作的执行结果。
struct ReminderActionResult {
    int affected_count = 0;
};

/// @brief 协调持久化提醒记录、一次性 Timing 任务、语音和通知。
class ScheduleReminderService final {
   public:
    using NowProvider = std::function<DateTime()>;

    /// @brief 构造提醒服务。
    ScheduleReminderService(ScheduleRepository& repository, ScheduleReminderTaskRepository& reminder_repository,
                            ScheduleService& schedule_service, ScheduleRuleService& rule_service,
                            timing::TimingTaskService& timing_service, ScheduleReminderSpeechPort& speech,
                            ScheduleReminderNotificationPort* notification = nullptr, NowProvider now_provider = {});

    /// @brief 启动提醒服务。
    /// @return 启动操作状态。
    Status Start();
    /// @brief 停止提醒服务。
    void Stop();
    /// @brief 同步指定日程的提醒。
    /// @param schedule_id 日程标识。
    /// @return 同步操作状态。
    Status SynchronizeSchedule(ScheduleId schedule_id);
    /// @brief 取消指定日程的提醒。
    /// @param schedule_id 日程标识。
    /// @return 取消操作状态。
    Status CancelScheduleReminder(ScheduleId schedule_id);
    /// @brief 暂停规则下的提醒。
    /// @param rule_id 规则标识。
    /// @return 暂停操作状态。
    Status SuspendRuleReminders(ScheduleRuleId rule_id);
    /// @brief 同步指定规则的提醒。
    /// @param rule_id 规则标识。
    /// @return 同步操作状态。
    Status SynchronizeRule(ScheduleRuleId rule_id);
    /// @brief 确认最近触发的提醒。
    /// @return 动作结果或错误状态。
    Result<ReminderActionResult> AcknowledgeRecentReminders();
    /// @brief 延后最近触发的提醒。
    /// @return 动作结果或错误状态。
    Result<ReminderActionResult> SnoozeRecentReminders();

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
    bool running_ = false;
    int64_t sequence_ = 0;
    int64_t chain_sequence_ = 0;
    std::unordered_map<ScheduleRuleId, RetryState> generation_retries_;
};

}  // namespace voicelife::schedule
