#pragma once

#include <mutex>
#include <vector>

#include "voicelife/application/calendar_application.h"

namespace voicelife::platform {

// This adapter is for architecture tests and early wiring only. It is not a persistence guarantee.
class InMemoryCalendarStore final : public application::CalendarStorePort {
   public:
    Result<std::optional<application::StoredCalendarEntry>> FindByRequestId(const std::string& request_id) override;
    Status SaveScheduleWithTimingTask(const application::StoredCalendarEntry& entry) override;
    [[nodiscard]] size_t Size() const;

   private:
    mutable std::mutex mutex_;
    std::vector<application::StoredCalendarEntry> entries_;
};

}  // namespace voicelife::platform
