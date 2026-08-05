#include "schedule_mock_data.h"

#include <chrono>

namespace voicelife::schedule {
namespace {

/** @brief 将固定 Unix 秒转换为日程模块使用的时间类型。 */
DateTime At(int64_t unix_seconds) { return DateTime{std::chrono::seconds{unix_seconds}}; }

}  // namespace

std::vector<Schedule> LoadMockSchedules() {
    // TODO(#134): 伪代码：database.query_schedules(query_conditions)。
    // 数据库适配器可用后，改为按查询条件读取日程，并删除这些固定模拟数据。
    return {
        Schedule{
            .id = 1001,
            .event = "模拟团队周会",
            .start_time = At(1'800'000'000),
            .end_time = At(1'800'003'600),
            .location = std::nullopt,
            .notes = std::nullopt,
            .reminder_id = std::nullopt,
            .status = ScheduleStatus::kActive,
            .created_at = At(1'799'900'000),
            .updated_at = At(1'799'900'000),
        },
        Schedule{
            .id = 1002,
            .event = "模拟单点日程",
            .start_time = At(1'800'007'200),
            .end_time = std::nullopt,
            .location = std::nullopt,
            .notes = std::nullopt,
            .reminder_id = std::nullopt,
            .status = ScheduleStatus::kActive,
            .created_at = At(1'799'900'000),
            .updated_at = At(1'799'900'000),
        },
    };
}

}  // namespace voicelife::schedule
