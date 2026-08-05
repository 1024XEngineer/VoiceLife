#include "voicelife/timing/timing_task_service.h"

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

Status ValidateCancelCommand(const CancelTimerTaskCommand& command) {
    if (command.task_id.empty() || command.schedule_id.empty()) {
        return Status::Error(ErrorCode::kInvalidArgument, "取消任务缺少 task_id 或 schedule_id");
    }
    if (!IsSupportedChangeScope(command.change_scope)) {
        return Status::Error(ErrorCode::kInvalidArgument, "不支持的取消范围");
    }

    switch (command.change_scope) {
        case ChangeScope::kSingle:
            if (command.instance_id.empty() || command.target_occurrence_at <= 0) {
                return Status::Error(ErrorCode::kInvalidArgument, "single 取消缺少目标 occurrence");
            }
            break;
        case ChangeScope::kFuture:
            if (command.effective_from <= 0) {
                return Status::Error(ErrorCode::kInvalidArgument, "future 取消缺少 effective_from");
            }
            break;
        case ChangeScope::kAll:
            break;
    }
    return Status::Ok();
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

Result<CancelTimerTaskResult> DefaultTimingTaskService::CancelTimerTask(const CancelTimerTaskCommand& command) {
    const Status validation = ValidateCancelCommand(command);
    if (!validation.ok()) {
        return Result<CancelTimerTaskResult>::Failure(validation.code, validation.message);
    }

    const auto existing_task = store_.FindTask(command.task_id);
    if (!existing_task.ok()) {
        return Result<CancelTimerTaskResult>::Failure(existing_task.status.code, existing_task.status.message);
    }
    if (existing_task.value->schedule_id != command.schedule_id) {
        return Result<CancelTimerTaskResult>::Failure(ErrorCode::kConflict, "任务不属于指定日程");
    }
    if (existing_task.value->status == TimingTaskStatus::kTerminated) {
        return Result<CancelTimerTaskResult>::Failure(ErrorCode::kConflict, "任务已终止");
    }
    if (command.change_scope == ChangeScope::kFuture && command.effective_from < existing_task.value->start_at) {
        return Result<CancelTimerTaskResult>::Failure(ErrorCode::kInvalidArgument, "future 取消边界早于任务开始时间");
    }

    const auto existing_instances = store_.ListInstances(command.task_id);
    if (!existing_instances.ok()) {
        return Result<CancelTimerTaskResult>::Failure(existing_instances.status.code,
                                                      existing_instances.status.message);
    }

    const int64_t now = clock_.Now();
    TimingTask updated_task = *existing_task.value;
    updated_task.updated_at = now;
    std::vector<TimerInstance> upsert_instances;
    std::string instance_id;
    int affected_instance_count = 0;

    switch (command.change_scope) {
        case ChangeScope::kSingle:
            for (const auto& instance : *existing_instances.value) {
                if (instance.id == command.instance_id && instance.planned_at == command.target_occurrence_at) {
                    if (!CanTransition(instance.status, TimerInstanceStatus::kSkipped)) {
                        return Result<CancelTimerTaskResult>::Failure(ErrorCode::kConflict, "目标 occurrence 不能取消");
                    }
                    TimerInstance cancelled_instance = instance;
                    cancelled_instance.status = TimerInstanceStatus::kSkipped;
                    cancelled_instance.last_action_at = now;
                    cancelled_instance.updated_at = now;
                    upsert_instances.push_back(std::move(cancelled_instance));
                    instance_id = command.instance_id;
                    affected_instance_count = 1;
                    break;
                }
            }
            if (upsert_instances.empty()) {
                return Result<CancelTimerTaskResult>::Failure(ErrorCode::kNotFound, "目标 occurrence 不存在");
            }
            break;
        case ChangeScope::kFuture:
            if (updated_task.effective_until != 0) {
                return Result<CancelTimerTaskResult>::Failure(ErrorCode::kConflict, "任务已设置未来取消边界");
            }
            updated_task.effective_until = command.effective_from - 1;
            if (updated_task.next_trigger_at >= command.effective_from) {
                updated_task.next_trigger_at = 0;
            }
            for (const auto& instance : *existing_instances.value) {
                if (instance.planned_at < command.effective_from) {
                    continue;
                }
                if (!CanTransition(instance.status, TimerInstanceStatus::kSkipped)) {
                    if (instance.status == TimerInstanceStatus::kCompleted ||
                        instance.status == TimerInstanceStatus::kSkipped) {
                        continue;
                    }
                    return Result<CancelTimerTaskResult>::Failure(ErrorCode::kConflict, "未来 occurrence 不能取消");
                }
                TimerInstance cancelled_instance = instance;
                cancelled_instance.status = TimerInstanceStatus::kSkipped;
                cancelled_instance.last_action_at = now;
                cancelled_instance.updated_at = now;
                upsert_instances.push_back(std::move(cancelled_instance));
            }
            affected_instance_count = static_cast<int>(upsert_instances.size());
            break;
        case ChangeScope::kAll:
            updated_task.status = TimingTaskStatus::kTerminated;
            updated_task.next_trigger_at = 0;
            updated_task.deleted_at = now;
            for (const auto& instance : *existing_instances.value) {
                if (!CanTransition(instance.status, TimerInstanceStatus::kSkipped)) {
                    continue;
                }
                TimerInstance cancelled_instance = instance;
                cancelled_instance.status = TimerInstanceStatus::kSkipped;
                cancelled_instance.last_action_at = now;
                cancelled_instance.updated_at = now;
                upsert_instances.push_back(std::move(cancelled_instance));
            }
            affected_instance_count = static_cast<int>(upsert_instances.size());
            break;
    }

    const Status saved = store_.UpdateTaskWithInstances({
        .task = updated_task,
        .upsert_instances = std::move(upsert_instances),
    });
    if (!saved.ok()) {
        return Result<CancelTimerTaskResult>::Failure(saved.code, saved.message);
    }

    return Result<CancelTimerTaskResult>::Success({
        .task_id = updated_task.id,
        .instance_id = std::move(instance_id),
        .status = updated_task.status,
        .affected_instance_count = affected_instance_count,
    });
}

}  // namespace voicelife::timing
