#include "voicelife/platform/in_memory_calendar_store.h"

namespace voicelife::platform {

Result<std::optional<application::StoredCalendarEntry>> InMemoryCalendarStore::FindByRequestId(
    const std::string& request_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& entry : entries_) {
        if (entry.schedule.request_id == request_id) {
            return Result<std::optional<application::StoredCalendarEntry>>::Success(entry);
        }
    }
    return Result<std::optional<application::StoredCalendarEntry>>::Success(std::nullopt);
}

Status InMemoryCalendarStore::SaveScheduleWithTimingTask(const application::StoredCalendarEntry& entry) {
    if (entry.schedule.id.empty() || entry.timing_task.id.empty() ||
        entry.schedule.id != entry.timing_task.schedule_id) {
        return Status::Error(ErrorCode::kInvalidArgument, "日程与定时任务关联不完整");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& existing : entries_) {
        if (existing.schedule.request_id == entry.schedule.request_id || existing.schedule.id == entry.schedule.id) {
            return Status::Error(ErrorCode::kConflict, "日程请求或标识已存在");
        }
    }
    entries_.push_back(entry);
    return Status::Ok();
}

size_t InMemoryCalendarStore::Size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

}  // namespace voicelife::platform
