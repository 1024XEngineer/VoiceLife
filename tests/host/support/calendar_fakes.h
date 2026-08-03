#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "voicelife/application/calendar_application.h"

namespace voicelife::test {

class FixedClock final : public application::ClockPort {
   public:
    explicit FixedClock(int64_t now) : now_(now) {}
    int64_t Now() const override { return now_; }

   private:
    int64_t now_;
};

class SequenceIds final : public application::IdGeneratorPort {
   public:
    std::string Next(const char* prefix) override { return std::string(prefix) + "-" + std::to_string(next_++); }

   private:
    int next_ = 1;
};

class RecordingNotifications final : public application::NotificationPort {
   public:
    Status Publish(const application::NotificationIntent& intent) override {
        last_intent = intent;
        ++count;
        return result;
    }

    Status result = Status::Ok();
    int count = 0;
    std::optional<application::NotificationIntent> last_intent;
};

class RecordingCalendarStore final : public application::CalendarStorePort {
   public:
    Result<std::optional<application::StoredCalendarEntry>> FindByRequestId(const std::string&) override {
        ++find_count;
        if (!find_result.ok()) {
            return Result<std::optional<application::StoredCalendarEntry>>::Failure(find_result.code,
                                                                                    find_result.message);
        }
        return Result<std::optional<application::StoredCalendarEntry>>::Success(existing);
    }

    Status SaveScheduleWithTimingTask(const application::StoredCalendarEntry& entry) override {
        ++save_count;
        saved = entry;
        return save_result;
    }

    Status find_result = Status::Ok();
    Status save_result = Status::Ok();
    std::optional<application::StoredCalendarEntry> existing;
    std::optional<application::StoredCalendarEntry> saved;
    int find_count = 0;
    int save_count = 0;
};

}  // namespace voicelife::test
