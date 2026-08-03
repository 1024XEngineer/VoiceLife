#pragma once

#include <cstdint>
#include <string>

#include "voicelife/contracts/status.h"

namespace voicelife::timing {

enum class TimingTaskStatus { kActive, kTerminated };

struct RegisterTimingTaskCommand {
    std::string schedule_id;
    int64_t starts_at = 0;
    std::string time_zone;
};

struct TimingTask {
    std::string id;
    std::string schedule_id;
    int64_t next_trigger_at = 0;
    std::string time_zone;
    TimingTaskStatus status = TimingTaskStatus::kActive;
    int64_t created_at = 0;
};

class TimingPolicy {
   public:
    Result<TimingTask> Register(const RegisterTimingTaskCommand& command, std::string task_id, int64_t now) const;
};

}  // namespace voicelife::timing
