#define main ExistingScheduleReminderTestMain
#include "schedule_reminder_service_test.cc"
#undef main

namespace {

/**
 * @brief 验证启动时会取消失去有效日程的持久化提醒任务。
 * @return 无。
 */
void CheckStartupCancelsOrphanedAndInactiveTasks() {
    ScriptedFixture fixture({MakeSchedule(2, "已完成日程", At(1'200), std::nullopt, ScheduleStatus::kCompleted)});
    const auto orphaned = fixture.reminder_repository.Insert({
        .schedule_id = 1,
        .chain_id = 11,
        .attempt = 1,
        .timing_task_id = "orphaned-reminder",
        .trigger_at = At(1'100),
        .triggered_at = std::nullopt,
        .created_at = At(900),
        .updated_at = At(900),
    });
    const auto inactive = fixture.reminder_repository.Insert({
        .schedule_id = 2,
        .chain_id = 12,
        .attempt = 1,
        .timing_task_id = "inactive-reminder",
        .trigger_at = At(1'200),
        .triggered_at = std::nullopt,
        .created_at = At(900),
        .updated_at = At(900),
    });
    Check(orphaned.ok() && inactive.ok(), "应准备孤立和非活动日程的提醒任务");

    Check(fixture.reminder.Start().ok(), "清理无效持久化提醒后服务应正常启动");
    const auto stored_orphaned = fixture.reminder_repository.FindById(orphaned.value->id);
    const auto stored_inactive = fixture.reminder_repository.FindById(inactive.value->id);
    Check(stored_orphaned.ok() && stored_inactive.ok(), "清理后的提醒任务应继续保留审计记录");
    Check(stored_orphaned.value->timer_status == ScheduleReminderTimerStatus::kCancelled &&
              stored_orphaned.value->business_status == ScheduleReminderBusinessStatus::kCancelled,
          "孤立提醒应转为已取消状态");
    Check(stored_inactive.value->timer_status == ScheduleReminderTimerStatus::kCancelled &&
              stored_inactive.value->business_status == ScheduleReminderBusinessStatus::kCancelled,
          "非活动日程提醒应转为已取消状态");
    Check(fixture.timing.register_commands.empty(), "无效提醒不应重新注册到定时服务");
}

/**
 * @brief 验证启动时会立即处理已经到期的持久化提醒。
 * @return 无。
 */
void CheckStartupTriggersExpiredPersistedTask() {
    ScriptedFixture fixture({MakeSchedule(1, "过期提醒", At(900))});
    const auto expired = fixture.reminder_repository.Insert({
        .schedule_id = 1,
        .chain_id = 21,
        .attempt = 3,
        .timing_task_id = "expired-reminder",
        .trigger_at = At(950),
        .triggered_at = std::nullopt,
        .created_at = At(900),
        .updated_at = At(900),
    });
    Check(expired.ok(), "应准备已经到期的持久化提醒");

    Check(fixture.reminder.Start().ok(), "到期提醒应在恢复阶段完成处理");
    const auto stored = fixture.reminder_repository.FindById(expired.value->id);
    Check(stored.ok() && stored.value->timer_status == ScheduleReminderTimerStatus::kTriggered &&
              stored.value->business_status == ScheduleReminderBusinessStatus::kExhausted,
          "第三次到期提醒应直接进入耗尽状态");
    Check(fixture.speech.texts.size() == 1, "恢复到期提醒应播报一次语音");
    Check(fixture.speech.texts.front() ==
              "提醒：现在是「过期提醒」时间了 这是最后一次提醒；之后不再创建新的推迟提醒。",
          "第三次提醒语音应追加最后一次稍后提醒说明");
    Check(fixture.timing.register_commands.empty(), "第三次提醒不应创建后续定时任务");
}

/**
 * @brief 验证确认和延迟动作会过滤无关状态并报告取消失败。
 * @return 无。
 */
void CheckActionFilteringAndCancellationFailure() {
    ScriptedFixture fixture({MakeSchedule(1, "动作边界", At(900))});
    const auto ignored = fixture.reminder_repository.Insert({
        .schedule_id = 1,
        .chain_id = 31,
        .attempt = 1,
        .timing_task_id = "ignored-trigger",
        .trigger_at = At(990),
        .business_status = ScheduleReminderBusinessStatus::kAcknowledged,
        .timer_status = ScheduleReminderTimerStatus::kTriggered,
        .triggered_at = At(995),
        .created_at = At(900),
        .updated_at = At(995),
    });
    Check(ignored.ok(), "应准备已确认的历史提醒");
    Check(!fixture.reminder.AcknowledgeRecentReminders().ok(), "已确认提醒不应再次参与确认");
    Check(!fixture.reminder.SnoozeRecentReminders().ok(), "没有默认后续任务时不应允许延迟");

    auto triggered = *ignored.value;
    triggered.id = 0;
    triggered.chain_id = 32;
    triggered.timing_task_id = "triggered-reminder";
    triggered.business_status = ScheduleReminderBusinessStatus::kWaitingAcknowledgement;
    Check(fixture.reminder_repository.Insert(triggered).ok(), "应准备等待确认的提醒");
    auto follow_up = triggered;
    follow_up.id = 0;
    follow_up.attempt = 2;
    follow_up.timing_task_id = "pending-follow-up";
    follow_up.trigger_at = At(1'500);
    follow_up.business_status = ScheduleReminderBusinessStatus::kScheduled;
    follow_up.timer_status = ScheduleReminderTimerStatus::kPending;
    follow_up.triggered_at = std::nullopt;
    Check(fixture.reminder_repository.Insert(follow_up).ok(), "应准备默认后续提醒");

    fixture.timing.cancel_acceptance = CommandAcceptance::kUnavailable;
    const auto acknowledged = fixture.reminder.AcknowledgeRecentReminders();
    Check(!acknowledged.ok() && acknowledged.status.code == ErrorCode::kUnavailable,
          "后续提醒取消命令未接收时确认动作应返回可重试错误");
    const auto stored_follow_up = fixture.reminder_repository.FindBySchedule(1);
    Check(stored_follow_up.ok() && std::any_of(stored_follow_up.value->begin(), stored_follow_up.value->end(),
                                               [](const auto& task) {
                                                   return task.timing_task_id == "pending-follow-up" &&
                                                          task.timer_status == ScheduleReminderTimerStatus::kPending;
                                               }),
          "取消失败时应保留待执行的后续提醒");
}

}  // namespace

int main() {
    CheckStartupCancelsOrphanedAndInactiveTasks();
    CheckStartupTriggersExpiredPersistedTask();
    CheckActionFilteringAndCancellationFailure();
    return 0;
}
