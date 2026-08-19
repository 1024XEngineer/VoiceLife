#define main ExistingScheduleReminderTestMain
#include "schedule_reminder_service_test.cc"
#undef main

namespace {

/** @brief 验证默认时间提供者和重复停止操作的公共行为。 @return 无。 */
void CheckDefaultClockAndIdempotentStop() {
    Fixture fixture({MakeSchedule(1, "默认时钟", At(1'100))});
    Check(fixture.reminder.Start().ok(), "默认时间提供者应允许服务启动");
    fixture.reminder.Stop();
    fixture.reminder.Stop();
    Check(fixture.timing.cancel_calls == 1, "重复停止不应重复取消提醒");
}

/** @brief 验证无效规则标识不会触发仓储或定时任务操作。 @return 无。 */
void CheckInvalidRuleIdentifiers() {
    Fixture fixture({});
    Check(!fixture.reminder.SuspendRuleReminders(0).ok(), "零规则标识应被拒绝");
    Check(!fixture.reminder.SynchronizeRule(-1).ok(), "负规则标识应被拒绝");
    Check(fixture.timing.cancel_calls == 0 && fixture.timing.register_commands.empty(),
          "无效规则标识不应操作定时服务");
}

}  // namespace

int main() {
    CheckDefaultClockAndIdempotentStop();
    CheckInvalidRuleIdentifiers();
    return 0;
}
