#include "voicelife/timing/timing_task.h"

namespace voicelife::timing {

bool CanTransition(TaskStatus from, TaskStatus to) {
    if (from == TaskStatus::kPending) {
        return to == TaskStatus::kExecuting || to == TaskStatus::kCancelled;
    }
    return from == TaskStatus::kExecuting && to == TaskStatus::kCompleted;
}

}  // namespace voicelife::timing
