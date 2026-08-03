#pragma once

#include <cstdint>
#include <string>

#include "voicelife/contracts/status.h"

namespace voicelife::schedule {

enum class ScheduleStatus { kActive, kCancelled };

struct CreateScheduleCommand {
    std::string request_id;
    std::string title;
    int64_t starts_at = 0;
    int64_t ends_at = 0;
    std::string time_zone = "Asia/Shanghai";
};

struct Schedule {
    std::string id;
    std::string request_id;
    std::string title;
    int64_t starts_at = 0;
    int64_t ends_at = 0;
    std::string time_zone;
    ScheduleStatus status = ScheduleStatus::kActive;
    int64_t created_at = 0;
};

class SchedulePolicy {
   public:
    Result<Schedule> Create(const CreateScheduleCommand& command, std::string schedule_id, int64_t now) const;
};

}  // namespace voicelife::schedule
