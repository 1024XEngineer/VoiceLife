#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace voicelife::timing {

using TimingTaskId = std::string;
using ScheduleId = std::string;

enum class TimingTaskStatus {
    kActive,
    kTerminated,
};

enum class RecurrenceFrequency {
    kNone,
    kDay,
    kWeek,
    kMonth,
    kYear,
}; //日、周、月、年

struct RecurrenceRule {
    RecurrenceFrequency frequency = RecurrenceFrequency::kNone;
    int64_t start_at = 0;
    std::string time_zone = "Asia/Shanghai";  //默认Asia/Shanghai时区，符合文档要求。
    std::vector<int> by_weekdays{};
    std::vector<int> by_month_days{};
    std::vector<int> by_months{};
};

struct TimingTask {
    TimingTaskId id{};
    ScheduleId schedule_id{};
    int64_t next_trigger_at = 0;
    std::string time_zone = "Asia/Shanghai";
    RecurrenceRule recurrence{};
    TimingTaskStatus status = TimingTaskStatus::kActive;
    int64_t created_at = 0;
    int64_t updated_at = 0;
    int64_t effective_until = 0;
    int64_t deleted_at = 0;
};

}  // namespace voicelife::timing
