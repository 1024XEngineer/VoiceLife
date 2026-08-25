#include "voicelife/storage_memory/memory_schedule_reminder_task_repository.h"

#include <chrono>
#include <cstdlib>
#include <iostream>

namespace {

using namespace voicelife;
using namespace voicelife::schedule;

DateTime At(int64_t seconds) { return DateTime{std::chrono::seconds{seconds}}; }

void Check(bool condition, const char* message) {
    if (condition) {
        return;
    }
    std::cerr << "FAILED: " << message << '\n';
    std::exit(1);
}

ScheduleReminderTask MakeTask(int64_t schedule_id, int64_t chain_id, int attempt, std::string timing_task_id) {
    return {
        .schedule_id = schedule_id,
        .chain_id = chain_id,
        .attempt = attempt,
        .timing_task_id = std::move(timing_task_id),
        .trigger_at = At(1'100 + attempt),
        .created_at = At(900),
        .updated_at = At(900),
    };
}

}  // namespace

int main() {
    using voicelife::storage_memory::MemoryScheduleReminderTaskRepository;

    MemoryScheduleReminderTaskRepository repository;
    const auto first = repository.Insert(MakeTask(1, 10, 1, "timing-1"));
    Check(first.ok() && first.value->id == 1 && first.value->attempt == 1, "插入必须分配提醒任务标识并保留字段");

    const auto second = repository.Insert(MakeTask(1, 10, 2, "timing-2"));
    const auto third = repository.Insert(MakeTask(2, 20, 1, "timing-3"));
    Check(second.ok() && third.ok(), "仓储必须保存多个日程和尝试次数");
    Check(repository.FindById(first.value->id).ok() && !repository.FindById(999).ok(),
          "按标识查询必须区分存在和不存在记录");
    Check(repository.FindByTimingTaskId("timing-1").ok() && !repository.FindByTimingTaskId("missing").ok(),
          "仓储必须支持按 Timing task 标识精确查询");
    Check(repository.FindBySchedule(1).value->size() == 2 && repository.FindBySchedule(999).value->empty() &&
              repository.FindAll().value->size() == 3,
          "仓储必须支持按日程和全量查询");

    auto triggered = *first.value;
    triggered.business_status = ScheduleReminderBusinessStatus::kWaitingAcknowledgement;
    triggered.timer_status = ScheduleReminderTimerStatus::kTriggered;
    triggered.triggered_at = At(1'200);
    triggered.updated_at = At(1'200);
    Check(repository.Update(triggered).ok(), "仓储必须更新已存在的提醒任务");
    const auto recent = repository.FindTriggered(At(1'190), At(1'200));
    Check(recent.ok() && recent.value->size() == 1 && recent.value->front().id == triggered.id,
          "触发查询必须包含窗口边界内等待确认的任务");
    triggered.business_status = ScheduleReminderBusinessStatus::kSnoozed;
    triggered.action_operation_id = "operation-1";
    triggered.action_kind = ScheduleReminderActionKind::kSnooze;
    triggered.action_occurred_at = At(1'201);
    triggered.action_next_trigger_at = At(1'800);
    Check(repository.Update(triggered).ok(), "仓储必须保存已提交的延迟动作结果");
    const auto persisted_action = repository.FindByTimingTaskId("timing-1");
    Check(persisted_action.ok() && persisted_action.value->action_operation_id == "operation-1" &&
              persisted_action.value->action_next_trigger_at == At(1'800),
          "仓储必须完整返回动作幂等键和下一次触发时间");

    auto exhausted = *second.value;
    exhausted.business_status = ScheduleReminderBusinessStatus::kExhausted;
    exhausted.timer_status = ScheduleReminderTimerStatus::kTriggered;
    exhausted.triggered_at = At(1'190);
    Check(repository.Update(exhausted).ok(), "仓储必须保存耗尽终态");
    Check(repository.FindTriggered(At(1'190), At(1'200)).value->size() == 1,
          "触发查询必须返回耗尽任务并排除已延迟终态");

    exhausted.business_status = ScheduleReminderBusinessStatus::kAcknowledged;
    Check(repository.Update(exhausted).ok(), "仓储必须保存确认终态");
    Check(repository.FindTriggered(At(1'190), At(1'200)).value->empty(), "触发查询必须排除已确认终态");

    Check(!repository.Insert(MakeTask(1, 10, 1, "timing-duplicate-attempt")).ok(),
          "同一提醒链不能重复保存相同尝试次数");
    Check(!repository.Insert(MakeTask(3, 30, 1, "timing-2")).ok(), "Timing task 标识必须唯一");
    auto duplicate_operation = MakeTask(3, 30, 1, "timing-operation-duplicate");
    duplicate_operation.business_status = ScheduleReminderBusinessStatus::kAcknowledged;
    duplicate_operation.action_operation_id = "operation-1";
    duplicate_operation.action_kind = ScheduleReminderActionKind::kAcknowledge;
    duplicate_operation.action_occurred_at = At(1'300);
    Check(!repository.Insert(duplicate_operation).ok(), "动作 operationId 必须唯一");

    auto cancelled = triggered;
    cancelled.business_status = ScheduleReminderBusinessStatus::kCancelled;
    cancelled.action_operation_id = std::nullopt;
    cancelled.action_kind = std::nullopt;
    cancelled.action_occurred_at = std::nullopt;
    cancelled.action_next_trigger_at = std::nullopt;
    Check(repository.Update(cancelled).ok(), "仓储必须保存取消终态");
    Check(repository.FindTriggered(At(1'190), At(1'200)).value->empty(), "触发查询必须排除已取消终态");

    Check(!repository.Insert(MakeTask(0, 30, 1, "invalid-schedule")).ok() &&
              !repository.Insert(MakeTask(3, 0, 1, "invalid-chain")).ok() &&
              !repository.Insert(MakeTask(3, 30, 0, "invalid-attempt-zero")).ok() &&
              !repository.Insert(MakeTask(3, 30, 4, "invalid-attempt-four")).ok(),
          "插入必须拒绝非法日程、提醒链和尝试次数");
    auto invalid_status = MakeTask(3, 30, 1, "invalid-status");
    invalid_status.business_status = static_cast<ScheduleReminderBusinessStatus>(99);
    Check(!repository.Insert(invalid_status).ok(), "插入必须拒绝非法业务状态");
    invalid_status = MakeTask(3, 30, 1, "invalid-timer-status");
    invalid_status.timer_status = static_cast<ScheduleReminderTimerStatus>(99);
    Check(!repository.Insert(invalid_status).ok(), "插入必须拒绝非法定时器状态");

    const auto check_invalid_action = [&repository](ScheduleReminderTask task, const char* message) {
        Check(repository.Insert(task).status.code == ErrorCode::kInvalidArgument, message);
    };
    auto invalid_action = MakeTask(4, 40, 1, "action-kind-without-operation");
    invalid_action.action_kind = ScheduleReminderActionKind::kAcknowledge;
    check_invalid_action(invalid_action, "动作类型不能脱离 operationId 单独保存");
    invalid_action = MakeTask(4, 40, 1, "empty-action-operation");
    invalid_action.business_status = ScheduleReminderBusinessStatus::kAcknowledged;
    invalid_action.action_operation_id = "";
    invalid_action.action_kind = ScheduleReminderActionKind::kAcknowledge;
    invalid_action.action_occurred_at = At(1'300);
    check_invalid_action(invalid_action, "动作 operationId 不能为空");
    invalid_action = MakeTask(4, 40, 1, "operation-without-kind");
    invalid_action.action_operation_id = "operation-without-kind";
    invalid_action.action_occurred_at = At(1'300);
    check_invalid_action(invalid_action, "operationId 必须配套动作类型");
    invalid_action = MakeTask(4, 40, 1, "operation-without-time");
    invalid_action.action_operation_id = "operation-without-time";
    invalid_action.action_kind = ScheduleReminderActionKind::kAcknowledge;
    check_invalid_action(invalid_action, "operationId 必须配套动作发生时间");
    invalid_action = MakeTask(4, 40, 1, "acknowledge-with-next-trigger");
    invalid_action.business_status = ScheduleReminderBusinessStatus::kAcknowledged;
    invalid_action.action_operation_id = "acknowledge-with-next-trigger";
    invalid_action.action_kind = ScheduleReminderActionKind::kAcknowledge;
    invalid_action.action_occurred_at = At(1'300);
    invalid_action.action_next_trigger_at = At(1'900);
    check_invalid_action(invalid_action, "确认动作不能保存下一次触发时间");
    invalid_action = MakeTask(4, 40, 1, "acknowledge-wrong-status");
    invalid_action.business_status = ScheduleReminderBusinessStatus::kSnoozed;
    invalid_action.action_operation_id = "acknowledge-wrong-status";
    invalid_action.action_kind = ScheduleReminderActionKind::kAcknowledge;
    invalid_action.action_occurred_at = At(1'300);
    check_invalid_action(invalid_action, "确认动作必须对应已确认业务状态");
    invalid_action = MakeTask(4, 40, 1, "snooze-without-next-trigger");
    invalid_action.business_status = ScheduleReminderBusinessStatus::kSnoozed;
    invalid_action.action_operation_id = "snooze-without-next-trigger";
    invalid_action.action_kind = ScheduleReminderActionKind::kSnooze;
    invalid_action.action_occurred_at = At(1'300);
    check_invalid_action(invalid_action, "延迟动作必须保存下一次触发时间");
    invalid_action = MakeTask(4, 40, 1, "snooze-wrong-status");
    invalid_action.business_status = ScheduleReminderBusinessStatus::kAcknowledged;
    invalid_action.action_operation_id = "snooze-wrong-status";
    invalid_action.action_kind = ScheduleReminderActionKind::kSnooze;
    invalid_action.action_occurred_at = At(1'300);
    invalid_action.action_next_trigger_at = At(1'900);
    check_invalid_action(invalid_action, "延迟动作必须对应已延迟业务状态");
    invalid_action = MakeTask(4, 40, 1, "unknown-action-kind");
    invalid_action.business_status = ScheduleReminderBusinessStatus::kAcknowledged;
    invalid_action.action_operation_id = "unknown-action-kind";
    invalid_action.action_kind = static_cast<ScheduleReminderActionKind>(99);
    invalid_action.action_occurred_at = At(1'300);
    check_invalid_action(invalid_action, "仓储必须拒绝未知动作类型");

    ScheduleReminderTask missing = MakeTask(1, 40, 1, "missing");
    missing.id = 999;
    Check(!repository.Update(missing).ok(), "更新不存在的提醒任务必须失败");

    auto duplicate_attempt = *third.value;
    duplicate_attempt.chain_id = 10;
    duplicate_attempt.attempt = 2;
    Check(!repository.Update(duplicate_attempt).ok(), "更新不能制造重复提醒链尝试次数");
    auto duplicate_timing_id = *third.value;
    duplicate_timing_id.timing_task_id = "timing-2";
    Check(!repository.Update(duplicate_timing_id).ok(), "更新不能制造重复 Timing task 标识");
    auto invalid = *third.value;
    invalid.attempt = 4;
    Check(!repository.Update(invalid).ok(), "更新必须拒绝非法任务字段");

    MemoryScheduleReminderTaskRepository default_time_repository;
    auto default_time_task = MakeTask(1, 50, 1, "default-time");
    default_time_task.created_at = DateTime{};
    default_time_task.updated_at = DateTime{};
    const auto default_time = default_time_repository.Insert(default_time_task);
    Check(default_time.ok() && default_time.value->created_at != DateTime{} &&
              default_time.value->updated_at == default_time.value->created_at,
          "未提供时间戳时仓储必须生成一致的创建和更新时间");
    return 0;
}
