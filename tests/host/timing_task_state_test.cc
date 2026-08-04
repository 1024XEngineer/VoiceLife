#include "support/test_support.h"
#include "voicelife/timing/timing_task_types.h"

using voicelife::test::Check;
using namespace voicelife::timing;

int main() {
    Check(CanTransition(TimerInstanceStatus::kPending, TimerInstanceStatus::kModified),
          "待处理实例应允许进入 modified");
    Check(!CanTransition(TimerInstanceStatus::kCompleted, TimerInstanceStatus::kTriggered),
          "已完成实例不应回退到 triggered");

    Check(!CanTransition(ReminderType::kWeak, ReminderTriggerStatus::kTriggered,
                         ReminderTriggerStatus::kSnoozed),
          "弱提醒不应允许 snooze");
    Check(CanTransition(ReminderType::kStrong, ReminderTriggerStatus::kTriggered,
                        ReminderTriggerStatus::kSnoozed),
          "强提醒应允许从 triggered 进入 snoozed");
    Check(!CanTransition(ReminderType::kStrong, ReminderTriggerStatus::kDismissed,
                         ReminderTriggerStatus::kTriggered),
          "已关闭提醒不应回退到 triggered");
    return 0;
}
