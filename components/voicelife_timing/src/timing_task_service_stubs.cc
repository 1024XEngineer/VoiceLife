#include <string>

#include "voicelife/timing/timing_task_service.h"

namespace voicelife::timing {
namespace {

template <typename T>
Result<T> NotImplemented(const char* operation) {
    return Result<T>::Failure(ErrorCode::kUnavailable, std::string(operation) + " 尚未实现");
}

}  // namespace

Result<ReminderTrigger> DefaultTimingTaskService::SnoozeReminderTrigger(const SnoozeReminderTriggerCommand&) {
    return NotImplemented<ReminderTrigger>("SnoozeReminderTrigger");
}

Result<ReminderTrigger> DefaultTimingTaskService::DismissReminderTrigger(const DismissReminderTriggerCommand&) {
    return NotImplemented<ReminderTrigger>("DismissReminderTrigger");
}

}  // namespace voicelife::timing
