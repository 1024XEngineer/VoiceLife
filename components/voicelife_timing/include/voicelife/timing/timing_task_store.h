#pragma once

#include "voicelife/timing/timing_task.h"

namespace voicelife::timing {

class TimingTaskStorePort {
   public:
    virtual ~TimingTaskStorePort() = default;
    // All methods are synchronous and transfer no ownership. Adapters must return only after commit.
    // Register and materialize are atomic and idempotent by task_id and (task_id, planned_at).
    // Duplicate identities return kConflict; unavailable storage returns kUnavailable.
    virtual Status RegisterTaskWithRules(const TimingTask&, const std::vector<ReminderRule>&) = 0;
    virtual Result<TimingTask> FindTask(const std::string& task_id) = 0;
    virtual Status UpdateTask(const TimingTask&) = 0;
    virtual Status UpdateTaskWithEvent(const TimingTask&, const TimingEvent&) = 0;
    virtual Result<std::vector<TimingTask>> ListTasks() = 0;
    virtual Result<std::vector<TimingTask>> ListDueTasks(int64_t now) = 0;
    virtual Status MaterializeOccurrence(const TimerInstance&, const std::vector<ReminderTrigger>&,
                                         const TimingTask& advanced_task, const TimingEvent&) = 0;
    virtual Status UpsertInstance(const TimerInstance&) = 0;
    virtual Result<std::optional<TimerInstance>> FindInstance(const std::string& instance_id) = 0;
    virtual Result<std::optional<TimerInstance>> FindInstanceByOccurrence(const std::string& task_id, int64_t planned_at) = 0;
    virtual Result<std::vector<TimerInstance>> ListInstances(const std::string& task_id) = 0;
    virtual Result<int> ApplyFutureUpdate(const TimingTask&, int64_t effective_from, int64_t now,
                                          const TimingEvent&) = 0;
    virtual Result<int> CancelFuture(const TimingTask&, int64_t effective_from, int64_t now,
                                     const TimingEvent&) = 0;
    virtual Status UpsertRules(const std::string& task_id, const std::vector<ReminderRule>&) = 0;
    virtual Result<int> DisableRuleAndCancelPendingTriggers(const std::string& rule_id, int64_t now) = 0;
    virtual Result<std::optional<ReminderRule>> FindRule(const std::string& rule_id) = 0;
    virtual Result<std::vector<ReminderRule>> ListRules(const std::string& task_id) = 0;
    virtual Status UpsertTriggers(const std::vector<ReminderTrigger>&) = 0;
    virtual Status UpdateTrigger(const ReminderTrigger&) = 0;
    virtual Status UpdateTriggerWithEvent(const ReminderTrigger&, const TimingEvent&) = 0;
    virtual Result<std::optional<ReminderTrigger>> FindTrigger(const std::string& trigger_id) = 0;
    virtual Result<std::vector<ReminderTrigger>> ListTriggers() = 0;
    virtual Result<std::vector<ReminderTrigger>> ListDueTriggers(int64_t now) = 0;
    // Events form a durable outbox. MarkEventPublished is idempotent by event_id.
    virtual Status EnqueueEvent(const TimingEvent&) = 0;
    virtual Result<std::vector<TimingEvent>> ListPendingEvents() = 0;
    virtual Status MarkEventPublished(const std::string& event_id) = 0;
};

class TimingClockPort { public: virtual ~TimingClockPort() = default; virtual int64_t Now() const = 0; };
class TimingIdGeneratorPort { public: virtual ~TimingIdGeneratorPort() = default; virtual std::string Next(const char* prefix) = 0; };
class TimingEventPort {
   public:
    virtual ~TimingEventPort() = default;
    // Downstream must process event_id idempotently. A successful return means ownership was accepted.
    virtual Status Publish(const TimingEvent&) = 0;
};

}  // namespace voicelife::timing
