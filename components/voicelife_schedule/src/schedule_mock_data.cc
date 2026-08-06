#include "schedule_mock_data.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <utility>

namespace voicelife::schedule {
namespace {

/**
 * @brief 将固定 Unix 秒转换为日程模块使用的时间类型。
 * @param unix_seconds Unix 时间戳秒数。
 * @return 对应的日程时间。
 */
DateTime At(int64_t unix_seconds) { return DateTime{std::chrono::seconds{unix_seconds}}; }

/**
 * @brief 创建模块默认的模拟日程集合。
 * @return 每次调用均返回一份独立的默认日程数据。
 */
std::vector<Schedule> MakeDefaultMockSchedules() {
    return {
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
}

/// 进程内共享的模拟日程存储及其同步状态。
struct MockScheduleStore {
    std::mutex mutex;
    std::vector<Schedule> schedules = MakeDefaultMockSchedules();
};

/**
 * @brief 返回进程内共享的模拟日程存储。
 * @return 可变的模拟日程存储。
 */
MockScheduleStore& Store() {
    static MockScheduleStore store;
    return store;
}

}  // namespace

std::vector<Schedule> LoadMockSchedules() {
    // 兼容修改日程接口使用的旧入口；数据来自同一份共享模拟存储。
    MockScheduleStore& store = Store();
    std::lock_guard<std::mutex> lock(store.mutex);
    return store.schedules;
}

std::vector<Schedule> LoadMockSchedulesForCreate() {
    // TODO(#134): 伪代码：database.query_active_schedules(command.start_time, command.end_time)。
    // 数据库适配器可用后，改为查询可能冲突或临近的有效日程，并删除这些固定模拟数据。
    return LoadMockSchedules();
}

Result<Schedule> FindMockScheduleById(ScheduleId schedule_id) {
    MockScheduleStore& store = Store();
    std::lock_guard<std::mutex> lock(store.mutex);
    const auto& schedules = store.schedules;
    const auto found = std::find_if(schedules.begin(), schedules.end(),
                                    [schedule_id](const Schedule& schedule) { return schedule.id == schedule_id; });
    if (found == schedules.end()) {
        return Result<Schedule>::Failure(ErrorCode::kNotFound, "未找到指定日程");
    }
    return Result<Schedule>::Success(*found);
}

Result<Schedule> RemoveMockSchedule(ScheduleId schedule_id) {
    MockScheduleStore& store = Store();
    std::lock_guard<std::mutex> lock(store.mutex);
    auto& schedules = store.schedules;
    const auto found = std::find_if(schedules.begin(), schedules.end(),
                                    [schedule_id](const Schedule& schedule) { return schedule.id == schedule_id; });
    if (found == schedules.end()) {
        return Result<Schedule>::Failure(ErrorCode::kNotFound, "未找到指定日程");
    }

    // 擦除前复制完整快照，供撤销服务记录撤销操作前的状态。
    Schedule removed = *found;
    schedules.erase(found);
    return Result<Schedule>::Success(std::move(removed));
}

Result<Schedule> RestoreMockSchedule(const Schedule& snapshot) {
    if (snapshot.id <= 0) {
        return Result<Schedule>::Failure(ErrorCode::kInvalidArgument, "日程 ID 必须为正整数");
    }

    MockScheduleStore& store = Store();
    std::lock_guard<std::mutex> lock(store.mutex);
    auto& schedules = store.schedules;
    const auto found = std::find_if(schedules.begin(), schedules.end(),
                                    [&snapshot](const Schedule& schedule) { return schedule.id == snapshot.id; });
    if (found == schedules.end()) {
        // 撤销删除时目标已不存在，需要按原快照重新插入。
        schedules.push_back(snapshot);
        return Result<Schedule>::Success(schedules.back());
    }

    // 撤销修改或撤销操作时完整覆盖，确保可空字段、状态和时间戳均恢复。
    *found = snapshot;
    return Result<Schedule>::Success(*found);
}

Result<Schedule> CancelMockSchedule(ScheduleId schedule_id) {
    // TODO(#134): 数据库适配器可用后，替换为带状态前置条件的原子 UPDATE。
    MockScheduleStore& store = Store();
    std::lock_guard<std::mutex> lock(store.mutex);
    for (Schedule& schedule : store.schedules) {
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

void ResetMockSchedulesForTesting() {
    MockScheduleStore& store = Store();
    std::lock_guard<std::mutex> lock(store.mutex);
    store.schedules = MakeDefaultMockSchedules();
}

void SeedMockSchedulesForTesting(std::vector<Schedule> schedules) {
    MockScheduleStore& store = Store();
    std::lock_guard<std::mutex> lock(store.mutex);
    store.schedules = std::move(schedules);
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
