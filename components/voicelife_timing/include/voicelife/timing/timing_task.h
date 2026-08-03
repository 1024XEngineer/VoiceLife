#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "voicelife/contracts/status.h"

namespace voicelife::timing {

enum class TimingTaskStatus { kActive, kTerminated };
enum class ChangeScope { kSingle, kFuture, kAll };
enum class RecurrenceFrequency { kNone, kDay, kWeek, kMonth, kYear };
enum class TimerInstanceStatus { kPending, kModified, kTriggered, kCompleted, kSkipped };
enum class ReminderType { kWeak, kStrong };
enum class ReminderRuleStatus { kActive, kDisabled };
enum class ReminderTriggerStatus { kPending, kTriggered, kDelivered, kSnoozed, kSkipped, kDismissed, kCancelled, kFailed };
enum class SortOrder { kAscending, kDescending };
enum class CalendarSortBy { kPlannedStartAt, kActualTriggerAt };
enum class TriggerSortBy { kActualTriggerAt, kPlannedTriggerAt, kCreatedAt };
enum class TimingEventType { kInstanceCreated, kReminderTriggered, kReminderSnoozed, kReminderDismissed, kTaskCancelled, kTaskUpdated };
enum class TimingEventStatus { kPending, kActive, kTriggered, kDelivered, kSnoozed, kDismissed, kTerminated };

struct RecurrenceRule {
    RecurrenceFrequency frequency = RecurrenceFrequency::kNone;
    int64_t start_at = 0;
    std::string time_zone = "Asia/Shanghai";
    std::vector<int> by_weekdays{};
    std::vector<int> by_month_days{};
    std::vector<int> by_months{};
};

struct RegisterTimingTaskCommand {
    std::string schedule_id{};
    int64_t starts_at = 0;
    std::string time_zone{};
};

struct TimingTask {
    std::string id{};
    std::string schedule_id{};
    int64_t next_trigger_at = 0;
    std::string time_zone{};
    RecurrenceRule recurrence{};
    std::optional<RecurrenceRule> pending_recurrence{};
    int64_t pending_effective_from = 0;
    TimingTaskStatus status = TimingTaskStatus::kActive;
    int64_t created_at = 0;
    int64_t updated_at = 0;
    int64_t effective_until = 0;
    int64_t deleted_at = 0;
};

struct InstanceOverrides {
    std::optional<int64_t> start_at{};
    std::optional<int64_t> end_at{};
};

struct TimerInstance {
    std::string id{};
    std::string task_id{};
    int64_t planned_at = 0;
    int64_t planned_end_at = 0;
    int64_t actual_trigger_at = 0;
    TimerInstanceStatus status = TimerInstanceStatus::kPending;
    InstanceOverrides override_fields{};
    int64_t last_action_at = 0;
    int64_t created_at = 0;
    int64_t updated_at = 0;
    int64_t deleted_at = 0;
};

struct ReminderRule {
    std::string id{};
    std::string task_id{};
    ReminderType type = ReminderType::kWeak;
    int offset_minutes = 0;
    int max_snooze_count = 0;
    int snooze_interval_minutes = 0;
    std::string channel = "voice";
    std::string source = "user_defined";
    ReminderRuleStatus status = ReminderRuleStatus::kActive;
    int64_t created_at = 0;
    int64_t updated_at = 0;
    int64_t deleted_at = 0;
};

struct ReminderTrigger {
    std::string id{};
    std::string reminder_rule_id{};
    std::string task_id{};
    std::string instance_id{};
    ReminderType type = ReminderType::kWeak;
    int64_t planned_trigger_at = 0;
    int64_t actual_trigger_at = 0;
    ReminderTriggerStatus status = ReminderTriggerStatus::kPending;
    int snooze_count = 0;
    int max_snooze_count = 0;
    int64_t delivered_at = 0;
    int64_t last_action_at = 0;
    std::string payload{};
    int64_t created_at = 0;
    int64_t updated_at = 0;
    int64_t deleted_at = 0;
};

struct TimingEvent {
    TimingEventType event_type = TimingEventType::kInstanceCreated;
    std::string event_id{};
    std::string task_id{};
    std::string instance_id{};
    std::string reminder_rule_id{};
    std::string reminder_trigger_id{};
    std::string schedule_id{};
    int64_t planned_at = 0;
    int64_t trigger_at = 0;
    TimingEventStatus status = TimingEventStatus::kPending;
    int64_t occurred_at = 0;
};

struct PendingTimingEvent {
    TimingEvent event{};
    bool published = false;
};

struct RegisterTimerTaskCommand {
    std::string schedule_id{};
    int64_t start_at = 0;
    std::string time_zone = "Asia/Shanghai";
    RecurrenceRule recurrence{};
};
struct TimerTaskResult { std::string task_id{}; TimingTaskStatus status = TimingTaskStatus::kActive; int64_t next_trigger_at = 0; };
struct UpdateTimerTaskCommand {
    std::string task_id{};
    std::string schedule_id{};
    ChangeScope scope = ChangeScope::kAll;
    int64_t start_at = 0;
    std::string instance_id{};
    int64_t target_occurrence_at = 0;
    int64_t effective_from = 0;
    std::optional<RecurrenceRule> recurrence{};
};
struct UpdateTimerTaskResult { std::string task_id{}; TimingTaskStatus status = TimingTaskStatus::kActive; int64_t next_trigger_at = 0; std::string instance_id{}; int affected_instance_count = 0; };
struct CancelTimerTaskCommand { std::string task_id{}; std::string schedule_id{}; ChangeScope scope = ChangeScope::kAll; std::string instance_id{}; int64_t target_occurrence_at = 0; int64_t effective_from = 0; };
struct CancelTimerTaskResult { std::string task_id{}; std::string instance_id{}; TimingTaskStatus status = TimingTaskStatus::kActive; int affected_instance_count = 0; };
struct CalendarViewQuery { int64_t range_start = 0; int64_t range_end = 0; std::string schedule_id{}; std::optional<TimerInstanceStatus> status{}; int page = 1; int page_size = 20; CalendarSortBy sort_by = CalendarSortBy::kPlannedStartAt; SortOrder sort_order = SortOrder::kAscending; };
struct CalendarOccurrence { std::string occurrence_id{}; std::string schedule_id{}; std::string task_id{}; std::string instance_id{}; std::string title{}; int64_t planned_start_at = 0; int64_t planned_end_at = 0; int64_t actual_trigger_at = 0; TimerInstanceStatus status = TimerInstanceStatus::kPending; bool is_recurring = false; bool is_exception = false; InstanceOverrides override_fields{}; };
struct CalendarView { std::vector<CalendarOccurrence> occurrences{}; int total = 0; int page = 1; int page_size = 20; bool has_more = false; };
struct ReminderTriggerQuery { std::string task_id{}; std::string instance_id{}; std::string schedule_id{}; std::optional<ReminderType> type{}; std::optional<ReminderTriggerStatus> status{}; int64_t range_start = 0; int64_t range_end = 0; int page = 1; int page_size = 20; TriggerSortBy sort_by = TriggerSortBy::kActualTriggerAt; SortOrder sort_order = SortOrder::kAscending; };
struct ReminderTriggerPage { std::vector<ReminderTrigger> triggers{}; int total = 0; int page = 1; int page_size = 20; bool has_more = false; };
struct SnoozeReminderTriggerCommand { std::string reminder_trigger_id{}; int delay_minutes = 0; };
struct DeleteReminderRuleResult { std::string reminder_rule_id{}; ReminderRuleStatus status = ReminderRuleStatus::kDisabled; int affected_trigger_count = 0; };

class TimingPolicy {
   public:
    Result<TimingTask> Register(const RegisterTimingTaskCommand& command, std::string task_id, int64_t now) const;
};

}  // namespace voicelife::timing
