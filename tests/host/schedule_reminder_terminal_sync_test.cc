#define main ExistingScheduleReminderTestMain
#include "schedule_reminder_service_test.cc"
#undef main

namespace {

/**
 * @brief 验证终态、无开始时间和已过期日程同步时不会注册提醒。
 * @return 无。
 */
void CheckTerminalScheduleSynchronization() {
    ScriptedFixture fixture({
        MakeSchedule(1, "已完成日程", At(1'200), std::nullopt, ScheduleStatus::kCompleted),
        MakeSchedule(2, "无开始时间", std::nullopt),
        MakeSchedule(3, "已过期日程", At(900)),
    });
    Check(fixture.reminder.Start().ok(), "终态同步测试应启动提醒服务");

    Check(fixture.reminder.SynchronizeSchedule(1).ok(), "已完成日程同步应幂等成功");
    Check(fixture.reminder.SynchronizeSchedule(2).ok(), "无开始时间日程同步应幂等成功");
    Check(fixture.reminder.SynchronizeSchedule(3).ok(), "已过期日程同步应幂等成功");
    Check(fixture.timing.register_commands.empty(), "无需提醒的日程不应注册定时任务");
}

}  // namespace

int main() {
    CheckTerminalScheduleSynchronization();
    return 0;
}
