#pragma once

#include <cstdint>
#include <string>

#include "voicelife/contracts/status.h"

namespace voicelife::timing {

/// Represents the lifecycle state of a timing task.
enum class TimingTaskStatus { kActive, kTerminated };

/// Supplies data required to register a timing task.
struct RegisterTimingTaskCommand {
    std::string schedule_id;
    int64_t starts_at = 0;
    std::string time_zone;
};

/// Stores the trigger and lifecycle state of a timing task.
struct TimingTask {
    std::string id;
    std::string schedule_id;
    int64_t next_trigger_at = 0;
    std::string time_zone;
    TimingTaskStatus status = TimingTaskStatus::kActive;
    int64_t created_at = 0;
};

/// Applies timing-domain validation and task construction rules.
class TimingPolicy {
   public:
    /**
     * @brief Registers the first timing task for a schedule.
     * @param command Schedule timing information to register.
     * @param task_id ID assigned to the new task.
     * @param now Current Unix timestamp in seconds.
     * @return Registered task or a validation error.
     */
    Result<TimingTask> Register(const RegisterTimingTaskCommand& command, std::string task_id, int64_t now) const;
};

}  // namespace voicelife::timing
