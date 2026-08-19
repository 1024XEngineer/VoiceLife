#define main ExistingScheduleReminderTestMain
#include "schedule_reminder_service_test.cc"
#undef main

namespace {

/** @brief 验证默认时间提供者和重复停止操作的公共行为。 @return 无。 */
void CheckDefaultClockAndIdempotentStop() {
    ScriptedFixture fixture({MakeSchedule(1, "默认时钟", std::nullopt, std::nullopt)});
    auto persisted = *fixture.repository.FindById(1).value;
    persisted.reminder_task_id = 77;
    Check(fixture.repository.Update(persisted).ok(), "应设置持久化提醒任务");
    ScheduleReminderService reminder(fixture.repository, fixture.schedule_service, fixture.rule_service, fixture.timing,
                                     fixture.speech);
    Check(reminder.Start().ok(), "默认时间提供者应允许服务启动");
    reminder.Stop();
    reminder.Stop();
    Check(fixture.timing.cancel_calls == 1, "重复停止不应重复取消提醒");
}

/** @brief 验证无效规则标识不会触发仓储或定时任务操作。 @return 无。 */
void CheckInvalidRuleIdentifiers() {
    ScriptedFixture fixture({});
    Check(!fixture.reminder.SuspendRuleReminders(0).ok(), "零规则标识应被拒绝");
    Check(!fixture.reminder.SynchronizeRule(-1).ok(), "负规则标识应被拒绝");
    Check(fixture.timing.cancel_calls == 0 && fixture.timing.register_commands.empty(), "无效规则标识不应操作定时服务");
}

}  // namespace

int main() {
    CheckDefaultClockAndIdempotentStop();
    CheckInvalidRuleIdentifiers();
    return 0;
}
