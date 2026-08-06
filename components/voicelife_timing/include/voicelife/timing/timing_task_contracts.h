#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "voicelife/timing/timing_task_types.h"

namespace voicelife::timing {

/// 表示查询结果的排序方向。
enum class SortOrder {
    kAscending,
    kDescending,
};

/// 表示日历视图支持的排序字段。
enum class CalendarSortBy {
    kPlannedStartAt,
    kActualTriggerAt,
};

/// 表示提醒触发查询支持的排序字段。
enum class TriggerSortBy {
    kActualTriggerAt,
    kPlannedTriggerAt,
    kCreatedAt,
};

/// 提供注册定时任务所需的数据；request_id 非空且用于重试幂等。
struct RegisterTimerTaskCommand {
    std::string request_id{};
    ScheduleId schedule_id{};
    int64_t start_at = 0;
    std::string time_zone = "Asia/Shanghai";
    RecurrenceRule recurrence{};
};

/// 返回新任务的标识、状态和下一次触发时间。
struct RegisterTimerTaskResult {
    TimingTaskId task_id{};
    TimingTaskStatus status = TimingTaskStatus::kActive;
    int64_t next_trigger_at = 0;
};

/// 提供按范围修改定时任务所需的数据。
struct UpdateTimerTaskCommand {
    TimingTaskId task_id{};
    ScheduleId schedule_id{};
    ChangeScope change_scope = ChangeScope::kAll;
    int64_t start_at = 0;
    std::string instance_id{};
    int64_t target_occurrence_at = 0;
    int64_t effective_from = 0;
    std::optional<RecurrenceRule> recurrence{};
};

/// 返回任务修改后的状态和受影响实例信息。
struct UpdateTimerTaskResult {
    TimingTaskId task_id{};
    TimingTaskStatus status = TimingTaskStatus::kActive;
    int64_t next_trigger_at = 0;
    std::string instance_id{};
    InstanceOverrides override_fields{};
    int affected_instance_count = 0;
};

/// 提供按范围取消定时任务所需的数据。
struct CancelTimerTaskCommand {
    TimingTaskId task_id{};
    ScheduleId schedule_id{};
    ChangeScope change_scope = ChangeScope::kAll;
    std::string instance_id{};
    int64_t target_occurrence_at = 0;
    int64_t effective_from = 0;
};

/// 返回任务取消后的状态和受影响实例数量。
struct CancelTimerTaskResult {
    TimingTaskId task_id{};
    std::string instance_id{};
    TimingTaskStatus status = TimingTaskStatus::kActive;
    int affected_instance_count = 0;
};

/// 提供调用方可设置的单条提醒规则字段。
struct ReminderRuleInput {
    std::string reminder_rule_id{};
    ReminderType type = ReminderType::kWeak;
    int offset_minutes = 0;
    int max_snooze_count = 0;
    int snooze_interval_minutes = 0;
    std::string channel = "voice";
    std::string source = "user_defined";
};

/// 提供批量创建或更新提醒规则所需的数据。
struct UpsertReminderRulesCommand {
    TimingTaskId task_id{};
    ScheduleId schedule_id{};
    std::vector<ReminderRuleInput> rules{};
};

/// 返回任务当前的完整提醒规则列表。
struct UpsertReminderRulesResult {
    TimingTaskId task_id{};
    std::vector<ReminderRule> reminder_rules{};
};

/// 提供关闭一条提醒规则所需的数据。
struct DeleteReminderRuleCommand {
    std::string reminder_rule_id{};
};

/// 返回规则关闭状态和受影响的未来触发数量。
struct DeleteReminderRuleResult {
    std::string reminder_rule_id{};
    ReminderRuleStatus status = ReminderRuleStatus::kDisabled;
    int affected_trigger_count = 0;
};

/// 提供日历视图的时间范围、过滤和分页条件。
struct CalendarViewQuery {
    int64_t range_start = 0;
    int64_t range_end = 0;
    ScheduleId schedule_id{};
    std::optional<TimerInstanceStatus> status{};
    int page = 1;
    int page_size = 20;
    CalendarSortBy sort_by = CalendarSortBy::kPlannedStartAt;
    SortOrder sort_order = SortOrder::kAscending;
};

/// 表示日历视图中的一次用户可见安排。
struct CalendarOccurrence {
    std::string occurrence_id{};
    ScheduleId schedule_id{};
    TimingTaskId task_id{};
    std::string instance_id{};
    std::string title{};
    int64_t planned_start_at = 0;
    int64_t planned_end_at = 0;
    int64_t actual_trigger_at = 0;
    TimerInstanceStatus status = TimerInstanceStatus::kPending;
    bool is_recurring = false;
    bool is_exception = false;
    InstanceOverrides override_fields{};
};

/// 返回分页后的日历 occurrence 列表。
struct CalendarView {
    std::vector<CalendarOccurrence> occurrences{};
    int total = 0;
    int page = 1;
    int page_size = 20;
    bool has_more = false;
};

/// 提供提醒触发的过滤、实际触发时间范围和分页条件；至少提供一项过滤，时间范围须成对出现且按左闭右开语义过滤。
struct ReminderTriggerQuery {
    TimingTaskId task_id{};
    std::string instance_id{};
    ScheduleId schedule_id{};
    std::optional<ReminderType> type{};
    std::optional<ReminderTriggerStatus> status{};
    int64_t range_start = 0;
    int64_t range_end = 0;
    int page = 1;
    int page_size = 20;
    TriggerSortBy sort_by = TriggerSortBy::kActualTriggerAt;
    SortOrder sort_order = SortOrder::kAscending;
};

/// 返回分页后的提醒触发列表。
struct ReminderTriggerPage {
    std::vector<ReminderTrigger> reminder_triggers{};
    int total = 0;
    int page = 1;
    int page_size = 20;
    bool has_more = false;
};

/// 提供推迟强提醒所需的数据。
struct SnoozeReminderTriggerCommand {
    std::string reminder_trigger_id{};
    int delay_minutes = 0;
};

/// 提供关闭强提醒所需的数据。
struct DismissReminderTriggerCommand {
    std::string reminder_trigger_id{};
};

}  // namespace voicelife::timing
