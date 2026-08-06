#include "schedule_mock_data.h"

#include <chrono>

namespace voicelife::schedule {
namespace {

/** @brief 将固定 Unix 秒转换为日程模块使用的时间类型。 */
DateTime At(int64_t unix_seconds) { return DateTime{std::chrono::seconds{unix_seconds}}; }

/** @brief 返回进程内共享的模拟日程集合。 @return 可变的模拟日程集合。 */
std::vector<Schedule>& MockSchedules() {
    static std::vector<Schedule> schedules{
        Schedule{
            .id = 1001,
            .event = "模拟团队周会",
            .start_time = At(1'800'000'000),
            .end_time = At(1'800'003'600),
            .location = std::nullopt,
            .notes = std::nullopt,
            .reminder_id = 2001,
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
    return schedules;
}

}  // namespace

std::vector<Schedule> LoadMockSchedules() {
    // 兼容修改日程接口使用的旧入口；数据来自同一份共享模拟存储。
    return MockSchedules();
}

std::vector<Schedule> LoadMockSchedulesForCreate() {
    // TODO(#134): 伪代码：database.query_active_schedules(command.start_time, command.end_time)。
    // 数据库适配器可用后，改为查询可能冲突或临近的有效日程，并删除这些固定模拟数据。
    return MockSchedules();
}

Result<Schedule> CancelMockSchedule(ScheduleId schedule_id) {
    // TODO(#134): 数据库适配器可用后，替换为带状态前置条件的原子 UPDATE。
    for (Schedule& schedule : MockSchedules()) {
        if (schedule.id != schedule_id) continue;
        if (schedule.status == ScheduleStatus::kCancelled) {
            return Result<Schedule>::Failure(ErrorCode::kConflict, "日程已取消，不能重复删除");
        }

        schedule.status = ScheduleStatus::kCancelled;
        schedule.updated_at = std::chrono::time_point_cast<std::chrono::seconds>(std::chrono::system_clock::now());
        return Result<Schedule>::Success(schedule);
    }
    return Result<Schedule>::Failure(ErrorCode::kNotFound, "未找到指定日程");
}

std::vector<Schedule> LoadMockSchedulesForQuery() {
    // TODO(#134)：数据库适配器可用后，改为按 QueryScheduleCommand 下推筛选和分页查询。
    return {
        Schedule{
            .id = 2001,
            .event = "数据库连接评审",
            .start_time = At(1'810'000'000),
            .end_time = At(1'810'003'600),
            .location = "会议室 A",
            .notes = std::nullopt,
            .reminder_id = std::nullopt,
            .status = ScheduleStatus::kActive,
            .created_at = At(1'809'900'000),
            .updated_at = At(1'809'900'000),
        },
        Schedule{
            .id = 2002,
            .event = "数据库连接复盘",
            .start_time = At(1'810'007'200),
            .end_time = std::nullopt,
            .location = "线上",
            .notes = std::nullopt,
            .reminder_id = std::nullopt,
            .status = ScheduleStatus::kCompleted,
            .created_at = At(1'809'900'100),
            .updated_at = At(1'810'008'000),
        },
        Schedule{
            .id = 2003,
            .event = "产品方案讨论",
            .start_time = At(1'810'003'600),
            .end_time = At(1'810'005'400),
            .location = "会议室 B",
            .notes = std::nullopt,
            .reminder_id = std::nullopt,
            .status = ScheduleStatus::kCancelled,
            .created_at = At(1'809'900'200),
            .updated_at = At(1'809'901'000),
        },
        Schedule{
            .id = 2004,
            .event = "整理周报",
            .start_time = std::nullopt,
            .end_time = std::nullopt,
            .location = std::nullopt,
            .notes = std::nullopt,
            .reminder_id = std::nullopt,
            .status = ScheduleStatus::kActive,
            .created_at = At(1'809'900'300),
            .updated_at = At(1'809'900'300),
        },
    };
}

}  // namespace voicelife::schedule
