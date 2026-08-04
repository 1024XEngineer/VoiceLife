#include "voicelife/timing/timing_task_types.h"

namespace voicelife::timing {

bool CanTransition(TimerInstanceStatus from, TimerInstanceStatus to) {
    switch (from) {
        case TimerInstanceStatus::kPending:
            return to == TimerInstanceStatus::kModified || to == TimerInstanceStatus::kTriggered ||
                   to == TimerInstanceStatus::kSkipped;
        case TimerInstanceStatus::kModified:
            return to == TimerInstanceStatus::kTriggered || to == TimerInstanceStatus::kSkipped;
        case TimerInstanceStatus::kTriggered:
            return to == TimerInstanceStatus::kCompleted || to == TimerInstanceStatus::kSkipped;
        case TimerInstanceStatus::kCompleted:
        case TimerInstanceStatus::kSkipped:
            return false;
    }
    return false;
}

bool CanTransition(ReminderType type, ReminderTriggerStatus from, ReminderTriggerStatus to) {
    switch (from) {
        case ReminderTriggerStatus::kPending:
            return to == ReminderTriggerStatus::kTriggered || to == ReminderTriggerStatus::kSkipped ||
                   to == ReminderTriggerStatus::kCancelled;
        case ReminderTriggerStatus::kTriggered:
            if (to == ReminderTriggerStatus::kDelivered || to == ReminderTriggerStatus::kFailed) {
                return true;
            }
            return type == ReminderType::kStrong &&
                   (to == ReminderTriggerStatus::kSnoozed || to == ReminderTriggerStatus::kDismissed);
        case ReminderTriggerStatus::kSnoozed:
            return type == ReminderType::kStrong &&
                   (to == ReminderTriggerStatus::kTriggered || to == ReminderTriggerStatus::kDismissed ||
                    to == ReminderTriggerStatus::kFailed);
        case ReminderTriggerStatus::kDelivered:
        case ReminderTriggerStatus::kSkipped:
        case ReminderTriggerStatus::kDismissed:
        case ReminderTriggerStatus::kCancelled:
        case ReminderTriggerStatus::kFailed:
            return false;
    }
    return false;
}

}  // namespace voicelife::timing
