#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "voicelife/contracts/status.h"
#include "voicelife/schedule/schedule.h"
#include "voicelife/timing/timing_task.h"

namespace voicelife::application {

struct StoredCalendarEntry {
    schedule::Schedule schedule;
    timing::TimingTask timing_task;
};

struct NotificationIntent {
    std::string event_id;
    std::string correlation_id;
    std::string kind;
    std::string schedule_id;
    std::string summary;
};

class CalendarStorePort {
   public:
    virtual ~CalendarStorePort() = default;
    virtual Result<std::optional<StoredCalendarEntry>> FindByRequestId(const std::string& request_id) = 0;

    // The adapter must persist both records atomically. Partial success is not allowed.
    // A duplicate request_id must return kConflict so the use case can resolve a concurrent replay.
    virtual Status SaveScheduleWithTimingTask(const StoredCalendarEntry& entry) = 0;
};

class NotificationPort {
   public:
    virtual ~NotificationPort() = default;
    virtual Status Publish(const NotificationIntent& intent) = 0;
};

class IdGeneratorPort {
   public:
    virtual ~IdGeneratorPort() = default;
    virtual std::string Next(const char* prefix) = 0;
};

class ClockPort {
   public:
    virtual ~ClockPort() = default;
    virtual int64_t Now() const = 0;
};

struct CreateScheduleOutcome {
    std::string schedule_id;
    std::string timing_task_id;
    bool duplicate = false;
    bool notification_accepted = false;
};

class CreateScheduleUseCase {
   public:
    virtual ~CreateScheduleUseCase() = default;
    virtual Result<CreateScheduleOutcome> CreateSchedule(const schedule::CreateScheduleCommand& command) = 0;
};

class CalendarApplication final : public CreateScheduleUseCase {
   public:
    CalendarApplication(CalendarStorePort& store, NotificationPort& notifications, IdGeneratorPort& ids,
                        ClockPort& clock)
        : store_(store), notifications_(notifications), ids_(ids), clock_(clock) {}

    Result<CreateScheduleOutcome> CreateSchedule(const schedule::CreateScheduleCommand& command) override;

   private:
    CalendarStorePort& store_;
    NotificationPort& notifications_;
    IdGeneratorPort& ids_;
    ClockPort& clock_;
    schedule::SchedulePolicy schedule_policy_;
    timing::TimingPolicy timing_policy_;
};

}  // namespace voicelife::application
