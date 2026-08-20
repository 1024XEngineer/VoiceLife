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

namespace voicelife::timing { class TimingTaskService; }

namespace voicelife::schedule {

class ScheduleReminderSpeechPort {
   public:
    virtual ~ScheduleReminderSpeechPort() = default;
    virtual Status SpeakScheduleReminder(std::string_view text) = 0;
};

/// Optional notification sink. IM adapters implement this outside the IM component.
class ScheduleReminderNotificationPort {
   public:
    virtual ~ScheduleReminderNotificationPort() = default;
    virtual Status SendScheduleReminder(const Schedule& schedule, const ScheduleReminderTask& task) = 0;
};

struct ReminderActionResult {
    int affected_count = 0;
};

/// Coordinates persistent reminder records, one-shot Timing tasks, speech and notifications.
class ScheduleReminderService final {
   public:
    using NowProvider = std::function<DateTime()>;

    ScheduleReminderService(ScheduleRepository& repository, ScheduleReminderTaskRepository& reminder_repository,
                            ScheduleService& schedule_service, ScheduleRuleService& rule_service,
                            timing::TimingTaskService& timing_service, ScheduleReminderSpeechPort& speech,
                            ScheduleReminderNotificationPort* notification = nullptr, NowProvider now_provider = {});

    Status Start();
    void Stop();
    Status SynchronizeSchedule(ScheduleId schedule_id);
    Status CancelScheduleReminder(ScheduleId schedule_id);
    Status SuspendRuleReminders(ScheduleRuleId rule_id);
    Status SynchronizeRule(ScheduleRuleId rule_id);
    Result<ReminderActionResult> AcknowledgeRecentReminders();
    Result<ReminderActionResult> SnoozeRecentReminders();

   private:
    struct RetryState { std::string task_id; int failure_count = 0; };

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
