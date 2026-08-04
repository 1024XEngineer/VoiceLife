#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace voicelife::timing {

using TimingTaskId = std::string;
using ScheduleId = std::string;

/// 表示定时任务的生命周期状态。
enum class TimingTaskStatus {
    kActive,
    kTerminated,
};

/// 表示任务支持的周期频率。
enum class RecurrenceFrequency {
    kNone,
    kDay,
    kWeek,
    kMonth,
    kYear,
};  // 日、周、月、年

/// 描述定时任务的周期展开规则。
struct RecurrenceRule {
    RecurrenceFrequency frequency = RecurrenceFrequency::kNone;
    int64_t start_at = 0;
    std::string time_zone = "Asia/Shanghai";
    std::vector<int> by_weekdays{};
    std::vector<int> by_month_days{};
    std::vector<int> by_months{};
};

/// 保存任务的调度规则、生命周期和下一次触发时间。
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
