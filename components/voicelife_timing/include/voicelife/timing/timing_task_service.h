#pragma once

#include "voicelife/timing/timing_task_store.h"

namespace voicelife::timing {

class TimingTaskService {
   public:
    virtual ~TimingTaskService() = default;
    virtual Result<TimerTaskResult> RegisterTimerTask(const RegisterTimerTaskCommand&) = 0;
    virtual Result<UpdateTimerTaskResult> UpdateTimerTask(const UpdateTimerTaskCommand&) = 0;
    virtual Result<CancelTimerTaskResult> CancelTimerTask(const CancelTimerTaskCommand&) = 0;
    virtual Result<std::vector<ReminderRule>> UpsertReminderRules(const std::string&, std::vector<ReminderRule>) = 0;
    virtual Result<DeleteReminderRuleResult> DeleteReminderRule(const std::string&) = 0;
    virtual Result<CalendarView> ListCalendarView(const CalendarViewQuery&) = 0;
    virtual Result<ReminderTriggerPage> ListReminderTriggers(const ReminderTriggerQuery&) = 0;
    virtual Result<ReminderTrigger> SnoozeReminderTrigger(const SnoozeReminderTriggerCommand&) = 0;
    virtual Result<ReminderTrigger> DismissReminderTrigger(const std::string& trigger_id) = 0;
};

class DefaultTimingTaskService final : public TimingTaskService {
   public:
    DefaultTimingTaskService(TimingTaskStorePort& store, TimingClockPort& clock, TimingIdGeneratorPort& ids,
                             TimingEventPort&)
        : store_(store), clock_(clock), ids_(ids) {}
    Result<TimerTaskResult> RegisterTimerTask(const RegisterTimerTaskCommand&) override;
    Result<UpdateTimerTaskResult> UpdateTimerTask(const UpdateTimerTaskCommand&) override;
    Result<CancelTimerTaskResult> CancelTimerTask(const CancelTimerTaskCommand&) override;
    Result<std::vector<ReminderRule>> UpsertReminderRules(const std::string&, std::vector<ReminderRule>) override;
    Result<DeleteReminderRuleResult> DeleteReminderRule(const std::string&) override;
    Result<CalendarView> ListCalendarView(const CalendarViewQuery&) override;
    Result<ReminderTriggerPage> ListReminderTriggers(const ReminderTriggerQuery&) override;
    Result<ReminderTrigger> SnoozeReminderTrigger(const SnoozeReminderTriggerCommand&) override;
    Result<ReminderTrigger> DismissReminderTrigger(const std::string&) override;

   private:
    TimingTaskStorePort& store_;
    TimingClockPort& clock_;
    TimingIdGeneratorPort& ids_;
};

}  // namespace voicelife::timing
