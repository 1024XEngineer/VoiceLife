#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace voicelife::timing {

using TimingTaskId = std::string;
using ScheduleId = std::string;

/// 表示定时任务的生命周期状态。
enum class TimingTaskStatus {
    kActive,
    kTerminated,
};

/// 表示修改或取消作用的 occurrence 范围。
enum class ChangeScope {
    kSingle,
    kFuture,
    kAll,
};

/// 表示任务支持的周期频率。
enum class RecurrenceFrequency {
    kNone,
    kDay,
    kWeek,
    kMonth,
    kYear,
};  // 日、周、月、年

/// 描述定时任务的周期展开规则，时区和周期锚点由所属任务统一提供。
struct RecurrenceRule {
    RecurrenceFrequency frequency = RecurrenceFrequency::kNone;
    std::vector<int> by_weekdays{};
    std::vector<int> by_month_days{};
    std::vector<int> by_months{};
};

/// 表示单次 occurrence 实例的生命周期状态。
enum class TimerInstanceStatus {
    kPending,
    kModified,
    kTriggered,
    kCompleted,
    kSkipped,
};

/// 区分自动结束的弱提醒与可交互的强提醒。
enum class ReminderType {
    kWeak,
    kStrong,
};

/// 表示提醒规则是否继续派生未来触发。
enum class ReminderRuleStatus {
    kActive,
    kDisabled,
};

/// 表示一次提醒动作的生命周期状态。
enum class ReminderTriggerStatus {
    kPending,
    kTriggered,
    kDelivered,
    kSnoozed,
    kSkipped,
    kDismissed,
    kCancelled,
    kFailed,
};

/// 表示 TimingTask 向下游发布的事件类型。
enum class TimingEventType {
    kInstanceCreated,
    kReminderTriggered,
    kReminderSnoozed,
    kReminderDismissed,
    kTaskCancelled,
    kTaskUpdated,
};

/// 表示下游事件携带的领域状态。
enum class TimingEventStatus {
    kPending,
    kActive,
    kTriggered,
    kDelivered,
    kSnoozed,
    kDismissed,
    kTerminated,
};

/// 保存任务的调度规则、生命周期和下一次触发时间。
struct TimingTask {
    TimingTaskId id{};
    ScheduleId schedule_id{};
    int64_t start_at = 0;
    int64_t next_trigger_at = 0;
    std::string time_zone = "Asia/Shanghai";
    RecurrenceRule recurrence{};
    TimingTaskStatus status = TimingTaskStatus::kActive;
    int64_t created_at = 0;
    int64_t updated_at = 0;
    int64_t effective_until = 0;
    int64_t deleted_at = 0;
};

/// 保存单次 occurrence 相对周期规则的覆盖字段。
struct InstanceOverrides {
    std::optional<int64_t> start_at{};
    std::optional<int64_t> end_at{};
};

/// 表示由定时任务派生的一次 occurrence。
struct TimerInstance {
    std::string id{};
    TimingTaskId task_id{};
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

/// 描述 occurrence 应派生的提醒动作规则。
struct ReminderRule {
    std::string id{};
    TimingTaskId task_id{};
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

/// 表示某个 occurrence 上的一次具体提醒动作。
struct ReminderTrigger {
    std::string id{};
    std::string reminder_rule_id{};
    TimingTaskId task_id{};
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

/// 定义 TimingTask 向 IM 或语音模块发布的最小事件契约。
struct TimingEvent {
    TimingEventType event_type = TimingEventType::kInstanceCreated;
    std::string event_id{};
    TimingTaskId task_id{};
    std::string instance_id{};
    std::string reminder_rule_id{};
    std::string reminder_trigger_id{};
    ScheduleId schedule_id{};
    int64_t planned_at = 0;
    int64_t trigger_at = 0;
    TimingEventStatus status = TimingEventStatus::kPending;
    std::string payload{};
    int64_t occurred_at = 0;
};

/**
 * @brief 判断实例状态流转是否符合领域约定。
 * @param from 当前状态。
 * @param to 目标状态。
 * @return 允许流转时返回 true。
 */
bool CanTransition(TimerInstanceStatus from, TimerInstanceStatus to);

/**
 * @brief 判断指定类型提醒的状态流转是否符合领域约定。
 * @param type 提醒类型。
 * @param from 当前状态。
 * @param to 目标状态。
 * @return 允许流转时返回 true。
 */
bool CanTransition(ReminderType type, ReminderTriggerStatus from, ReminderTriggerStatus to);

}  // namespace voicelife::timing
