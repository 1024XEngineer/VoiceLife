#define main ExistingScheduleReminderTestMain
#include "schedule_reminder_service_test.cc"
#undef main

namespace {

using voicelife::schedule::ReminderActionCommand;
using voicelife::schedule::ReminderActionResult;
using voicelife::schedule::ScheduleReminderActionKind;
using voicelife::schedule::ScheduleReminderNotificationPort;

ScheduleReminderTask TriggeredTask(int64_t schedule_id, int64_t chain_id, std::string timing_task_id) {
    return {.schedule_id = schedule_id,
            .chain_id = chain_id,
            .attempt = 1,
            .timing_task_id = std::move(timing_task_id),
            .trigger_at = At(1'100),
            .business_status = ScheduleReminderBusinessStatus::kWaitingAcknowledgement,
            .timer_status = ScheduleReminderTimerStatus::kTriggered,
            .triggered_at = At(1'100),
            .created_at = At(900),
            .updated_at = At(1'100)};
}

ScheduleReminderTask FollowUpTask(int64_t schedule_id, int64_t chain_id, std::string timing_task_id) {
    return {.schedule_id = schedule_id,
            .chain_id = chain_id,
            .attempt = 2,
            .timing_task_id = std::move(timing_task_id),
            .trigger_at = At(1'700),
            .business_status = ScheduleReminderBusinessStatus::kScheduled,
            .timer_status = ScheduleReminderTimerStatus::kPending,
            .created_at = At(1'100),
            .updated_at = At(1'100)};
}

ReminderActionCommand Action(std::string operation_id, std::string trigger_id, ScheduleReminderActionKind kind) {
    return {.operation_id = std::move(operation_id),
            .reminder_trigger_id = std::move(trigger_id),
            .action = kind,
            .snooze_minutes = kind == ScheduleReminderActionKind::kSnooze ? std::optional<int>{10} : std::nullopt};
}

void SeedActionChain(ScriptedFixture& fixture, int64_t schedule_id, int64_t chain_id, std::string trigger_id,
                     bool with_follow_up = true) {
    Check(fixture.reminder_repository.Insert(TriggeredTask(schedule_id, chain_id, trigger_id)).ok(),
          "动作测试应保存已触发提醒");
    if (with_follow_up) {
        Check(fixture.reminder_repository
                  .Insert(FollowUpTask(schedule_id, chain_id, "follow-up-" + std::to_string(chain_id)))
                  .ok(),
              "动作测试应保存默认后续提醒");
    }
}

void CheckActionValidationAndConflicts() {
    ScriptedFixture fixture({MakeSchedule(1, "动作校验", At(1'100)), MakeSchedule(2, "冲突动作", At(1'100))},
                            At(1'101));
    SeedActionChain(fixture, 1, 11, "trigger-1");
    SeedActionChain(fixture, 2, 22, "trigger-2");

    auto command = Action({}, "trigger-1", ScheduleReminderActionKind::kAcknowledge);
    Check(fixture.reminder.ExecuteReminderAction(command).status.code == ErrorCode::kInvalidArgument,
          "动作应拒绝空 operationId");
    command = Action("operation", {}, ScheduleReminderActionKind::kAcknowledge);
    Check(fixture.reminder.ExecuteReminderAction(command).status.code == ErrorCode::kInvalidArgument,
          "动作应拒绝空 reminderTriggerId");
    command = Action("operation", "trigger-1", static_cast<ScheduleReminderActionKind>(99));
    Check(fixture.reminder.ExecuteReminderAction(command).status.code == ErrorCode::kInvalidArgument,
          "动作应拒绝未知类型");
    command = Action("operation", "trigger-1", ScheduleReminderActionKind::kAcknowledge);
    command.snooze_minutes = 10;
    Check(fixture.reminder.ExecuteReminderAction(command).status.code == ErrorCode::kInvalidArgument,
          "确认动作不能携带延迟分钟数");
    command = Action("operation", "trigger-1", ScheduleReminderActionKind::kSnooze);
    command.snooze_minutes = std::nullopt;
    Check(fixture.reminder.ExecuteReminderAction(command).status.code == ErrorCode::kInvalidArgument,
          "延迟动作必须携带分钟数");
    command.snooze_minutes = 5;
    Check(fixture.reminder.ExecuteReminderAction(command).status.code == ErrorCode::kInvalidArgument,
          "延迟动作只能使用约定分钟数");
    Check(
        fixture.reminder.ExecuteReminderAction(Action("operation", "missing", ScheduleReminderActionKind::kAcknowledge))
                .status.code == ErrorCode::kNotFound,
        "动作应按 triggerId 精确寻址");

    const auto snoozed =
        fixture.reminder.ExecuteReminderAction(Action("operation-1", "trigger-1", ScheduleReminderActionKind::kSnooze));
    Check(snoozed.ok() && snoozed.value->next_trigger_at == At(1'700) && !snoozed.value->replayed,
          "延迟动作应返回默认后续提醒时间");
    const auto replayed =
        fixture.reminder.ExecuteReminderAction(Action("operation-1", "trigger-1", ScheduleReminderActionKind::kSnooze));
    Check(
        replayed.ok() && replayed.value->replayed && replayed.value->next_trigger_at == snoozed.value->next_trigger_at,
        "相同动作应重放持久化结果");
    Check(
        fixture.reminder.ExecuteReminderAction(Action("operation-2", "trigger-1", ScheduleReminderActionKind::kSnooze))
            .ok(),
        "同一提醒的相同动作应允许不同 operationId 幂等重放");
    Check(fixture.reminder
                  .ExecuteReminderAction(Action("operation-ack", "trigger-1", ScheduleReminderActionKind::kAcknowledge))
                  .status.code == ErrorCode::kAlreadyExists,
          "同一提醒的不同动作仍必须拒绝");

    ScriptedFixture acknowledge_fixture({MakeSchedule(3, "重复确认", At(1'100))}, At(1'101));
    SeedActionChain(acknowledge_fixture, 3, 33, "ack-trigger");
    const auto acknowledged = acknowledge_fixture.reminder.ExecuteReminderAction(
        Action("operation-ack-1", "ack-trigger", ScheduleReminderActionKind::kAcknowledge));
    const auto replayed_acknowledge = acknowledge_fixture.reminder.ExecuteReminderAction(
        Action("operation-ack-2", "ack-trigger", ScheduleReminderActionKind::kAcknowledge));
    Check(acknowledged.ok() && !acknowledged.value->replayed && replayed_acknowledge.ok() &&
              replayed_acknowledge.value->replayed && replayed_acknowledge.value->next_trigger_at == std::nullopt,
          "确认动作应允许跨入口幂等重放且不产生下一次提醒");
    Check(fixture.reminder
                  .ExecuteReminderAction(Action("operation-1", "trigger-2", ScheduleReminderActionKind::kAcknowledge))
                  .status.code == ErrorCode::kAlreadyExists,
          "operationId 不能跨提醒复用");
}

void CheckTerminalAndCancellationFailures() {
    ScriptedFixture terminal({MakeSchedule(1, "终态提醒", At(1'100))}, At(1'101));
    auto terminal_task = TriggeredTask(1, 31, "terminal-trigger");
    terminal_task.business_status = ScheduleReminderBusinessStatus::kCancelled;
    Check(terminal.reminder_repository.Insert(terminal_task).ok(), "应保存无动作记录的终态提醒");
    Check(terminal.reminder
                  .ExecuteReminderAction(
                      Action("terminal-operation", "terminal-trigger", ScheduleReminderActionKind::kAcknowledge))
                  .status.code == ErrorCode::kAlreadyExists,
          "不可操作终态应拒绝新动作");

    ScriptedFixture no_follow_up({MakeSchedule(2, "无后续提醒", At(1'100))}, At(1'101));
    SeedActionChain(no_follow_up, 2, 32, "no-follow-up", false);
    Check(
        no_follow_up.reminder
                .ExecuteReminderAction(Action("snooze-operation", "no-follow-up", ScheduleReminderActionKind::kSnooze))
                .status.code == ErrorCode::kNotFound,
        "没有默认后续提醒时不能延迟");

    ScriptedFixture unavailable({MakeSchedule(3, "取消不可用", At(1'100))}, At(1'101));
    SeedActionChain(unavailable, 3, 33, "unavailable-trigger");
    unavailable.timing.cancel_acceptance = CommandAcceptance::kUnavailable;
    Check(unavailable.reminder
                  .ExecuteReminderAction(
                      Action("ack-operation", "unavailable-trigger", ScheduleReminderActionKind::kAcknowledge))
                  .status.code == ErrorCode::kUnavailable,
          "确认动作应传播后续提醒取消失败");

    ScriptedFixture mixed({MakeSchedule(4, "部分成功", At(1'100)), MakeSchedule(5, "部分失败", At(1'100))}, At(1'101));
    SeedActionChain(mixed, 4, 44, "mixed-success", true);
    SeedActionChain(mixed, 5, 55, "mixed-failure", false);
    const auto mixed_result = mixed.reminder.ExecuteRecentReminderActions(ScheduleReminderActionKind::kSnooze);
    Check(!mixed_result.ok() && mixed_result.status.code == ErrorCode::kNotFound,
          "批量动作只要有一项失败就不能伪报整体成功");
}

void CheckPersistedVoiceActionRecoveryFacts() {
    ScriptedFixture fixture({MakeSchedule(4, "重启补报", At(1'100))}, At(1'101));
    SeedActionChain(fixture, 4, 44, "voice-trigger");
    const auto result = fixture.reminder.ExecuteReminderAction(
        Action("voice-action-44", "voice-trigger", ScheduleReminderActionKind::kSnooze));
    Check(result.ok(), "语音动作应先成功写入本地事实");
    const auto persisted = fixture.reminder.ListPersistedVoiceActionResults();
    Check(persisted.ok() && persisted.value->size() == 1 &&
              persisted.value->front().operation_id == "voice-action-44" &&
              persisted.value->front().next_trigger_at == At(1'700),
          "重启恢复扫描必须保留语音动作 operationId 与 nextTriggerAt");
}

class ImmediateActionNotification final : public ScheduleReminderNotificationPort {
   public:
    Status SendScheduleReminder(const Schedule&, const ScheduleReminderTask& task) override {
        const auto tasks = service_tasks();
        follow_up_visible = tasks.ok() && tasks.value->size() == 2;
        result = service->ExecuteReminderAction(Action(operation_id, *task.timing_task_id, action));
        return result.ok() ? Status::Ok() : result.status;
    }

    ScheduleReminderService* service = nullptr;
    std::function<Result<std::vector<ScheduleReminderTask>>()> service_tasks;
    std::string operation_id;
    ScheduleReminderActionKind action = ScheduleReminderActionKind::kAcknowledge;
    Result<ReminderActionResult> result = Result<ReminderActionResult>::Failure(ErrorCode::kInternal, "未执行");
    bool follow_up_visible = false;
};

struct NotificationFixture {
    explicit NotificationFixture(Schedule schedule)
        : repository({std::move(schedule)}),
          rules(repository),
          rule_service(rules, exceptions, repository),
          schedule_service(repository),
          reminder(repository, reminder_repository, schedule_service, rule_service, timing, speech, &notification,
                   [this]() { return now; }) {
        notification.service = &reminder;
        notification.service_tasks = [this]() { return reminder_repository.FindBySchedule(1); };
    }

    InMemoryScheduleRepository repository;
    voicelife::storage_memory::MemoryScheduleReminderTaskRepository reminder_repository;
    FakeExceptionRepository exceptions;
    FakeRuleRepository rules;
    ScheduleRuleService rule_service;
    ScheduleService schedule_service;
    ScriptedTimingService timing;
    FakeSpeech speech;
    ImmediateActionNotification notification;
    DateTime now = At(1'000);
    ScheduleReminderService reminder;
};

void FireFirstReminder(NotificationFixture& fixture) {
    Check(fixture.reminder.Start().ok(), "同步通知测试应启动提醒服务");
    Check(fixture.timing.register_commands.size() == 1, "启动时应注册首次提醒");
    const auto callback = fixture.timing.register_commands.front().callback;
    const auto task_id = voicelife::timing::TaskId::Create(fixture.timing.register_commands.front().task_id.Value());
    fixture.now = At(1'100);
    callback(*task_id, Trigger(1'100));
}

void CheckImmediateNotificationActions() {
    NotificationFixture snooze(MakeSchedule(1, "立即延迟", At(1'100)));
    snooze.notification.operation_id = "immediate-snooze";
    snooze.notification.action = ScheduleReminderActionKind::kSnooze;
    FireFirstReminder(snooze);
    const auto snoozed_tasks = snooze.reminder_repository.FindBySchedule(1);
    Check(snooze.notification.follow_up_visible && snooze.notification.result.ok() &&
              snooze.notification.result.value->next_trigger_at == At(1'700),
          "通知内立即延迟时默认后续提醒必须已经落库");
    Check(snoozed_tasks.ok() && snoozed_tasks.value->size() == 2 &&
              snoozed_tasks.value->front().business_status == ScheduleReminderBusinessStatus::kSnoozed,
          "立即延迟后不得额外创建后续提醒");

    NotificationFixture acknowledge(MakeSchedule(1, "立即确认", At(1'100)));
    acknowledge.notification.operation_id = "immediate-acknowledge";
    acknowledge.notification.action = ScheduleReminderActionKind::kAcknowledge;
    FireFirstReminder(acknowledge);
    const auto acknowledged_tasks = acknowledge.reminder_repository.FindBySchedule(1);
    Check(acknowledge.notification.follow_up_visible && acknowledge.notification.result.ok() &&
              acknowledge.timing.cancel_calls == 1,
          "通知内立即确认应取消已经注册的默认后续提醒");
    Check(acknowledged_tasks.ok() && acknowledged_tasks.value->size() == 2 &&
              acknowledged_tasks.value->back().timer_status == ScheduleReminderTimerStatus::kCancelled &&
              acknowledge.repository.FindById(1).value->status == ScheduleStatus::kCompleted,
          "立即确认后不得残留或重新创建待执行后续提醒");
}

}  // namespace

int main() {
    CheckActionValidationAndConflicts();
    CheckTerminalAndCancellationFailures();
    CheckPersistedVoiceActionRecoveryFacts();
    CheckImmediateNotificationActions();
    return 0;
}
