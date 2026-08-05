#include "voicelife/timing/timing_task_service.h"

#include <limits>
#include <utility>

#include "voicelife/timing/timing_task.h"

namespace voicelife::timing {
namespace {

bool ValuesInRange(const std::vector<int>& values, int minimum, int maximum) {
    for (const int value : values) {
        if (value < minimum || value > maximum) {
            return false;
        }
    }
    return true;
}

bool SameRecurrence(const RecurrenceRule& left, const RecurrenceRule& right) {
    return left.frequency == right.frequency && left.by_weekdays == right.by_weekdays &&
           left.by_month_days == right.by_month_days && left.by_months == right.by_months;
}

bool MatchesRegistration(const TimingTask& task, const RegisterTimerTaskCommand& command) {
    return task.request_id == command.request_id && task.schedule_id == command.schedule_id &&
           task.start_at == command.start_at && task.time_zone == command.time_zone &&
           SameRecurrence(task.recurrence, command.recurrence);
}

Result<RegisterTimerTaskResult> RegistrationResult(const TimingTask& task) {
    return Result<RegisterTimerTaskResult>::Success({
        .task_id = task.id,
        .status = task.status,
        .next_trigger_at = task.next_trigger_at,
    });
}

bool IsSupportedChangeScope(ChangeScope scope) {
    switch (scope) {
        case ChangeScope::kSingle:
        case ChangeScope::kFuture:
        case ChangeScope::kAll:
            return true;
    }
    return false;
}

Status ValidateRecurrence(const RecurrenceRule& recurrence) {
    const bool has_weekdays = !recurrence.by_weekdays.empty();
    const bool has_month_days = !recurrence.by_month_days.empty();
    const bool has_months = !recurrence.by_months.empty();

    switch (recurrence.frequency) {
        case RecurrenceFrequency::kNone:
        case RecurrenceFrequency::kDay:
            if (has_weekdays || has_month_days || has_months) {
                return Status::Error(ErrorCode::kInvalidArgument, "当前周期频率不接受筛选字段");
            }
            break;
        case RecurrenceFrequency::kWeek:
            if (has_month_days || has_months || !ValuesInRange(recurrence.by_weekdays, 1, 7)) {
                return Status::Error(ErrorCode::kInvalidArgument, "每周规则的筛选字段或星期值无效");
            }
            break;
        case RecurrenceFrequency::kMonth:
            if (has_weekdays || has_months || !ValuesInRange(recurrence.by_month_days, 1, 31)) {
                return Status::Error(ErrorCode::kInvalidArgument, "每月规则的筛选字段或日期值无效");
            }
            break;
        case RecurrenceFrequency::kYear:
            if (has_weekdays || !ValuesInRange(recurrence.by_month_days, 1, 31) ||
                !ValuesInRange(recurrence.by_months, 1, 12)) {
                return Status::Error(ErrorCode::kInvalidArgument, "每年规则的筛选字段、月份或日期值无效");
            }
            break;
    }
    return Status::Ok();
}

Status ValidateUpdateCommand(const UpdateTimerTaskCommand& command) {
    if (command.task_id.empty() || command.schedule_id.empty()) {
        return Status::Error(ErrorCode::kInvalidArgument, "修改任务缺少 task_id 或 schedule_id");
    }
    if (!IsSupportedChangeScope(command.change_scope)) {
        return Status::Error(ErrorCode::kInvalidArgument, "不支持的修改范围");
    }
    if (command.start_at <= 0) {
        return Status::Error(ErrorCode::kInvalidArgument, "修改任务缺少新的开始时间");
    }

    switch (command.change_scope) {
        case ChangeScope::kSingle:
            if (command.instance_id.empty() || command.target_occurrence_at <= 0) {
                return Status::Error(ErrorCode::kInvalidArgument, "single 修改缺少目标 occurrence");
            }
            if (command.recurrence.has_value()) {
                return Status::Error(ErrorCode::kInvalidArgument, "single 修改不能改变任务周期规则");
            }
            break;
        case ChangeScope::kFuture:
            if (command.effective_from <= 0) {
                return Status::Error(ErrorCode::kInvalidArgument, "future 修改缺少 effective_from");
            }
            break;
        case ChangeScope::kAll:
            break;
    }

    if (command.recurrence.has_value()) {
        return ValidateRecurrence(*command.recurrence);
    }
    return Status::Ok();
}

std::vector<TimerInstance> SkipMutableInstancesFrom(const std::vector<TimerInstance>& instances, int64_t boundary,
                                                    int64_t now) {
    std::vector<TimerInstance> skipped_instances;
    for (const auto& instance : instances) {
        if (instance.planned_at < boundary || !CanTransition(instance.status, TimerInstanceStatus::kSkipped)) {
            continue;
        }

        TimerInstance skipped = instance;
        skipped.status = TimerInstanceStatus::kSkipped;
        skipped.last_action_at = now;
        skipped.updated_at = now;
        skipped_instances.push_back(std::move(skipped));
    }
    return skipped_instances;
}

Result<TimerInstance> BuildSingleOverride(const UpdateTimerTaskCommand& command, const TimingTask& task,
                                          const std::vector<TimerInstance>& instances, int64_t now) {
    for (const auto& existing : instances) {
        if (existing.id == command.instance_id) {
            if (existing.planned_at != command.target_occurrence_at) {
                return Result<TimerInstance>::Failure(ErrorCode::kConflict, "目标实例不属于指定 occurrence");
            }
            if (existing.status != TimerInstanceStatus::kModified &&
                !CanTransition(existing.status, TimerInstanceStatus::kModified)) {
                return Result<TimerInstance>::Failure(ErrorCode::kConflict, "目标 occurrence 已不可修改");
            }

            TimerInstance instance = existing;
            instance.planned_at = command.target_occurrence_at;
            instance.status = TimerInstanceStatus::kModified;
            instance.override_fields.start_at = command.start_at;
            instance.last_action_at = now;
            instance.updated_at = now;
            return Result<TimerInstance>::Success(std::move(instance));
        }
    }

    return Result<TimerInstance>::Success({
        .id = command.instance_id,
        .task_id = task.id,
        .planned_at = command.target_occurrence_at,
        .status = TimerInstanceStatus::kModified,
        .override_fields = {.start_at = command.start_at},
        .last_action_at = now,
        .created_at = now,
        .updated_at = now,
    });
}

}  // namespace

Result<RegisterTimerTaskResult> DefaultTimingTaskService::RegisterTimerTask(const RegisterTimerTaskCommand& command) {
    if (command.request_id.empty()) {
        return Result<RegisterTimerTaskResult>::Failure(ErrorCode::kInvalidArgument, "注册请求缺少 request_id");
    }

    const RegisterTimingTaskCommand policy_command{
        .schedule_id = command.schedule_id,
        .starts_at = command.start_at,
        .time_zone = command.time_zone,
    };
    const Status policy_status = TimingPolicy{}.Validate(policy_command);
    if (!policy_status.ok()) {
        return Result<RegisterTimerTaskResult>::Failure(policy_status.code, policy_status.message);
    }

    const Status recurrence_status = ValidateRecurrence(command.recurrence);
    if (!recurrence_status.ok()) {
        return Result<RegisterTimerTaskResult>::Failure(recurrence_status.code, recurrence_status.message);
    }

    const auto existing = store_.FindTaskByRequestId(command.request_id);
    if (existing.ok()) {
        if (!MatchesRegistration(*existing.value, command)) {
            return Result<RegisterTimerTaskResult>::Failure(ErrorCode::kConflict, "request_id 已用于不同的注册请求");
        }
        return RegistrationResult(*existing.value);
    }
    if (existing.status.code != ErrorCode::kNotFound) {
        return Result<RegisterTimerTaskResult>::Failure(existing.status.code, existing.status.message);
    }

    auto task = TimingPolicy{}.Register(policy_command, ids_.NextTaskId(), clock_.Now());
    if (!task.ok()) {
        return Result<RegisterTimerTaskResult>::Failure(task.status.code, task.status.message);
    }

    task.value->request_id = command.request_id;
    task.value->recurrence = command.recurrence;
    task.value->updated_at = task.value->created_at;

    ReminderRule weak_rule{};
    weak_rule.id = ids_.NextReminderRuleId();
    weak_rule.task_id = task.value->id;
    weak_rule.type = ReminderType::kWeak;
    weak_rule.offset_minutes = -10;
    weak_rule.channel = "voice";
    weak_rule.source = "system_default";
    weak_rule.created_at = task.value->created_at;
    weak_rule.updated_at = task.value->created_at;

    ReminderRule strong_rule{};
    strong_rule.id = ids_.NextReminderRuleId();
    strong_rule.task_id = task.value->id;
    strong_rule.type = ReminderType::kStrong;
    strong_rule.max_snooze_count = 3;
    strong_rule.snooze_interval_minutes = 5;
    strong_rule.channel = "voice";
    strong_rule.source = "system_default";
    strong_rule.created_at = task.value->created_at;
    strong_rule.updated_at = task.value->created_at;

    const Status saved = store_.RegisterTaskWithRules(*task.value, {weak_rule, strong_rule});
    if (!saved.ok()) {
        if (saved.code == ErrorCode::kConflict) {
            const auto replay = store_.FindTaskByRequestId(command.request_id);
            if (replay.ok()) {
                if (!MatchesRegistration(*replay.value, command)) {
                    return Result<RegisterTimerTaskResult>::Failure(ErrorCode::kConflict,
                                                                    "request_id 已用于不同的注册请求");
                }
                return RegistrationResult(*replay.value);
            }
        }
        return Result<RegisterTimerTaskResult>::Failure(saved.code, saved.message);
    }

    return RegistrationResult(*task.value);
}

Result<UpdateTimerTaskResult> DefaultTimingTaskService::UpdateTimerTask(const UpdateTimerTaskCommand& command) {
    const Status validation = ValidateUpdateCommand(command);
    if (!validation.ok()) {
        return Result<UpdateTimerTaskResult>::Failure(validation.code, validation.message);
    }

    const auto existing_task = store_.FindTask(command.task_id);
    if (!existing_task.ok()) {
        return Result<UpdateTimerTaskResult>::Failure(existing_task.status.code, existing_task.status.message);
    }
    if (existing_task.value->schedule_id != command.schedule_id) {
        return Result<UpdateTimerTaskResult>::Failure(ErrorCode::kConflict, "任务不属于指定日程");
    }
    if (existing_task.value->status != TimingTaskStatus::kActive || existing_task.value->deleted_at != 0) {
        return Result<UpdateTimerTaskResult>::Failure(ErrorCode::kConflict, "任务已终止或删除，不能修改");
    }

    const auto existing_instances = store_.ListInstances(command.task_id);
    if (!existing_instances.ok()) {
        return Result<UpdateTimerTaskResult>::Failure(existing_instances.status.code,
                                                      existing_instances.status.message);
    }

    TimingTask updated_task = *existing_task.value;
    std::vector<TimerInstance> upsert_instances;
    std::string instance_id;
    InstanceOverrides override_fields;
    int affected_instance_count = 0;
    const int64_t now = clock_.Now();

    switch (command.change_scope) {
        case ChangeScope::kSingle: {
            auto instance = BuildSingleOverride(command, updated_task, *existing_instances.value, now);
            if (!instance.ok()) {
                return Result<UpdateTimerTaskResult>::Failure(instance.status.code, instance.status.message);
            }
            instance_id = instance.value->id;
            override_fields = instance.value->override_fields;
            affected_instance_count = 1;
            upsert_instances.push_back(std::move(*instance.value));
            break;
        }
        case ChangeScope::kFuture:
            updated_task.start_at = command.start_at;
            updated_task.next_trigger_at = command.start_at;
            updated_task.updated_at = now;
            if (command.recurrence.has_value()) {
                updated_task.recurrence = *command.recurrence;
            }
            upsert_instances = SkipMutableInstancesFrom(*existing_instances.value, command.effective_from, now);
            affected_instance_count = static_cast<int>(upsert_instances.size());
            break;
        case ChangeScope::kAll:
            updated_task.start_at = command.start_at;
            updated_task.next_trigger_at = command.start_at;
            updated_task.updated_at = now;
            if (command.recurrence.has_value()) {
                updated_task.recurrence = *command.recurrence;
            }
            upsert_instances =
                SkipMutableInstancesFrom(*existing_instances.value, std::numeric_limits<int64_t>::min(), now);
            affected_instance_count = static_cast<int>(upsert_instances.size());
            break;
    }

    const Status saved = store_.UpdateTaskWithInstances({
        .task = updated_task,
        .upsert_instances = std::move(upsert_instances),
    });
    if (!saved.ok()) {
        return Result<UpdateTimerTaskResult>::Failure(saved.code, saved.message);
    }

    return Result<UpdateTimerTaskResult>::Success({
        .task_id = updated_task.id,
        .status = updated_task.status,
        .next_trigger_at = updated_task.next_trigger_at,
        .instance_id = std::move(instance_id),
        .override_fields = override_fields,
        .affected_instance_count = affected_instance_count,
    });
}

}  // namespace voicelife::timing
