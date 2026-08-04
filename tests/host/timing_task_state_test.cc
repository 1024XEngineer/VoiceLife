#include "support/test_support.h"
#include "voicelife/timing/timing_task_types.h"

using voicelife::test::Check;
using namespace voicelife::timing;

int main() {
    Check(CanTransition(TimerInstanceStatus::kPending, TimerInstanceStatus::kModified),
          "待处理实例应允许进入 modified");
    Check(CanTransition(TimerInstanceStatus::kModified, TimerInstanceStatus::kTriggered),
          "已修改实例应允许进入 triggered");
    Check(CanTransition(TimerInstanceStatus::kTriggered, TimerInstanceStatus::kCompleted),
          "已触发实例应允许进入 completed");
    Check(!CanTransition(TimerInstanceStatus::kCompleted, TimerInstanceStatus::kTriggered),
          "已完成实例不应回退到 triggered");
    Check(!CanTransition(TimerInstanceStatus::kSkipped, TimerInstanceStatus::kTriggered),
          "已跳过实例不应进入 triggered");

    Check(CanTransition(ReminderType::kWeak, ReminderTriggerStatus::kPending, ReminderTriggerStatus::kTriggered),
          "待处理提醒应允许进入 triggered");
    Check(CanTransition(ReminderType::kWeak, ReminderTriggerStatus::kTriggered, ReminderTriggerStatus::kDelivered),
          "已触发提醒应允许进入 delivered");
    Check(CanTransition(ReminderType::kWeak, ReminderTriggerStatus::kTriggered, ReminderTriggerStatus::kFailed),
          "已触发提醒应允许进入 failed");
    Check(!CanTransition(ReminderType::kWeak, ReminderTriggerStatus::kTriggered, ReminderTriggerStatus::kSnoozed),
          "弱提醒不应允许 snooze");
    Check(CanTransition(ReminderType::kStrong, ReminderTriggerStatus::kTriggered, ReminderTriggerStatus::kSnoozed),
          "强提醒应允许从 triggered 进入 snoozed");
    Check(CanTransition(ReminderType::kStrong, ReminderTriggerStatus::kSnoozed, ReminderTriggerStatus::kTriggered),
          "已推迟强提醒应允许再次进入 triggered");
    Check(!CanTransition(ReminderType::kWeak, ReminderTriggerStatus::kSnoozed, ReminderTriggerStatus::kTriggered),
          "弱提醒不应从 snoozed 进入 triggered");
    Check(!CanTransition(ReminderType::kStrong, ReminderTriggerStatus::kDelivered, ReminderTriggerStatus::kTriggered),
          "已送达提醒不应回退到 triggered");
    Check(!CanTransition(ReminderType::kStrong, ReminderTriggerStatus::kSkipped, ReminderTriggerStatus::kTriggered),
          "已跳过提醒不应进入 triggered");
    Check(!CanTransition(ReminderType::kStrong, ReminderTriggerStatus::kDismissed, ReminderTriggerStatus::kTriggered),
          "已关闭提醒不应回退到 triggered");
    Check(!CanTransition(ReminderType::kStrong, ReminderTriggerStatus::kCancelled, ReminderTriggerStatus::kTriggered),
          "已取消提醒不应进入 triggered");
    Check(!CanTransition(ReminderType::kStrong, ReminderTriggerStatus::kFailed, ReminderTriggerStatus::kTriggered),
          "失败提醒不应回退到 triggered");
    return 0;
}
