#include <chrono>
#include <string>

#include "support/in_memory_schedule_repository.h"
#include "support/test_support.h"
#include "voicelife/schedule/schedule_service.h"

using voicelife::ErrorCode;
using voicelife::schedule::CreateScheduleCommand;
using voicelife::schedule::DateTime;
using voicelife::schedule::ScheduleService;
using voicelife::test::Check;
using voicelife::test::InMemoryScheduleRepository;

namespace {

/** @brief 将测试 Unix 秒转换为日程时间。 */
DateTime At(int64_t unix_seconds) { return DateTime{std::chrono::seconds{unix_seconds}}; }

/** @brief 验证日程名称规范化和长度限制。 */
void CheckEventValidation(const ScheduleService& service) {
    CreateScheduleCommand no_time;
    no_time.event = "  阅读  ";
    const auto no_time_result = service.create_schedule(no_time);
    Check(no_time_result.status.ok() && no_time_result.schedule->event == "阅读", "创建时应清理名称两端空白");
    Check(no_time_result.message == "日程创建成功", "无临近日程时应返回普通成功消息");
    Check(no_time_result.conflicts.empty() && no_time_result.nearby_schedules.empty(), "无时间日程不应产生时间提示");

    CreateScheduleCommand empty;
    empty.event = " \t\n ";
    const auto empty_result = service.create_schedule(empty);
    Check(empty_result.status.code == ErrorCode::kInvalidArgument && !empty_result.error.empty(),
          "空白日程名称应返回参数错误");

    CreateScheduleCommand too_long;
    too_long.event = std::string(101, 'a');
    Check(service.create_schedule(too_long).status.code == ErrorCode::kInvalidArgument, "超过一百字符的名称应被拒绝");

    CreateScheduleCommand utf8_too_long;
    for (int index = 0; index < 101; ++index) utf8_too_long.event += "日";
    Check(service.create_schedule(utf8_too_long).status.code == ErrorCode::kInvalidArgument,
          "中文名称应按字符数量执行一百字符限制");
}

/** @brief 验证开始时间和结束时间的组合规则。 */
void CheckTimeValidation(const ScheduleService& service) {
    CreateScheduleCommand end_only;
    end_only.event = "结束时间非法";
    end_only.end_time = At(1'800'000'000);
    Check(service.create_schedule(end_only).status.code == ErrorCode::kInvalidArgument, "只提供结束时间应被拒绝");

    CreateScheduleCommand invalid_range;
    invalid_range.event = "时间范围非法";
    invalid_range.start_time = At(1'800'000'100);
    invalid_range.end_time = At(1'800'000'100);
    Check(service.create_schedule(invalid_range).status.code == ErrorCode::kInvalidArgument,
          "结束时间不晚于开始时间应被拒绝");
}

/** @brief 验证区间冲突和忽略冲突行为。 */
void CheckIntervalConflicts(const ScheduleService& service) {
    CreateScheduleCommand conflict;
    conflict.event = "冲突日程";
    conflict.start_time = At(1'800'000'600);
    conflict.end_time = At(1'800'001'200);
    const auto conflict_result = service.create_schedule(conflict);
    Check(conflict_result.status.code == ErrorCode::kConflict && !conflict_result.schedule.has_value(),
          "默认应拒绝冲突日程");
    Check(conflict_result.conflicts.size() == 1 && !conflict_result.error.empty(), "冲突结果应返回已有日程和错误信息");

    conflict.ignore_conflict = true;
    const auto ignored_result = service.create_schedule(conflict);
    Check(ignored_result.status.ok() && ignored_result.schedule.has_value() && ignored_result.conflicts.size() == 1,
          "忽略冲突时应创建并保留冲突提示");

    CreateScheduleCommand point_inside_interval;
    point_inside_interval.event = "区间内时间点";
    point_inside_interval.start_time = At(1'800'001'800);
    Check(service.create_schedule(point_inside_interval).status.code == ErrorCode::kConflict,
          "落在已有区间内的单点日程应冲突");

    CreateScheduleCommand interval_over_point;
    interval_over_point.event = "覆盖时间点";
    interval_over_point.start_time = At(1'800'006'900);
    interval_over_point.end_time = At(1'800'007'500);
    Check(service.create_schedule(interval_over_point).status.code == ErrorCode::kConflict,
          "覆盖已有单点日程的时间区间应冲突");
}

/** @brief 验证首尾相接和十五分钟临近日程规则。 */
void CheckNearbySchedules(const ScheduleService& service, InMemoryScheduleRepository& repository) {
    CreateScheduleCommand adjacent;
    adjacent.event = "首尾相接";
    adjacent.start_time = At(1'800'003'600);
    adjacent.end_time = At(1'800'004'200);
    const auto adjacent_result = service.create_schedule(adjacent);
    Check(adjacent_result.status.ok() && adjacent_result.conflicts.empty(), "首尾相接不应视为冲突");
    Check(adjacent_result.nearby_schedules.size() == 1, "首尾相接的已有日程应作为临近日程返回");

    // 每个断言场景使用同一份固定初始数据，避免前一个创建结果影响附近数量。
    repository.Reset(InMemoryScheduleRepository::DefaultSchedules());
    CreateScheduleCommand fifteen_minutes;
    fifteen_minutes.event = "十五分钟边界";
    fifteen_minutes.start_time = At(1'800'004'500);
    fifteen_minutes.end_time = At(1'800'005'100);
    const auto nearby_result = service.create_schedule(fifteen_minutes);
    Check(nearby_result.status.ok() && nearby_result.nearby_schedules.size() == 1, "相距十五分钟的不冲突日程应被返回");
    Check(nearby_result.message == "日程创建成功，附近还有其他日程", "临近日程应反映在成功消息中");

    repository.Reset(InMemoryScheduleRepository::DefaultSchedules());
    CreateScheduleCommand outside_window;
    outside_window.event = "临近范围外";
    outside_window.start_time = At(1'800'004'501);
    outside_window.end_time = At(1'800'005'101);
    Check(service.create_schedule(outside_window).nearby_schedules.empty(), "超过十五分钟的日程不应作为临近日程返回");
}

/** @brief 验证无结束时间日程之间的冲突规则。 */
void CheckPointConflicts(const ScheduleService& service) {
    CreateScheduleCommand point_conflict;
    point_conflict.event = "同一时间点";
    point_conflict.start_time = At(1'800'007'200);
    Check(service.create_schedule(point_conflict).status.code == ErrorCode::kConflict, "开始时间相同的单点日程应冲突");
}

}  // namespace

int main() {
    {
        InMemoryScheduleRepository repository(InMemoryScheduleRepository::DefaultSchedules());
        const ScheduleService service(repository, repository);
        CheckEventValidation(service);
    }
    {
        InMemoryScheduleRepository repository(InMemoryScheduleRepository::DefaultSchedules());
        const ScheduleService service(repository, repository);
        CheckTimeValidation(service);
    }
    {
        InMemoryScheduleRepository repository(InMemoryScheduleRepository::DefaultSchedules());
        const ScheduleService service(repository, repository);
        CheckIntervalConflicts(service);
    }
    {
        InMemoryScheduleRepository repository(InMemoryScheduleRepository::DefaultSchedules());
        const ScheduleService service(repository, repository);
        CheckNearbySchedules(service, repository);
    }
    {
        InMemoryScheduleRepository repository(InMemoryScheduleRepository::DefaultSchedules());
        const ScheduleService service(repository, repository);
        CheckPointConflicts(service);
    }
    return 0;
}
