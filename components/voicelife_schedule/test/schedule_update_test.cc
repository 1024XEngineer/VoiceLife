#include <chrono>
#include <optional>
#include <string>

#include "support/test_support.h"
#include "voicelife/contracts/status.h"
#include "voicelife/schedule/schedule_service.h"

using voicelife::ErrorCode;
using voicelife::schedule::DateTime;
using voicelife::schedule::ScheduleService;
using voicelife::schedule::ScheduleStatus;
using voicelife::schedule::UpdateScheduleCommand;
using voicelife::test::Check;

namespace {

/**
 * @brief 将固定 Unix 秒转换为日程模块使用的时间类型。
 * @param unix_seconds Unix 时间戳秒数。
 * @return 日程模块的秒级时间点。
 */
DateTime At(int64_t unix_seconds) { return DateTime{std::chrono::seconds{unix_seconds}}; }

/**
 * @brief 校验普通字段修改及字段清空语义。
 * @param service 被测试的日程服务。
 * @return 无返回值。
 */
void CheckFieldUpdates(ScheduleService& service) {
    UpdateScheduleCommand command;
    command.schedule_id = 1001;
    command.event = "  更新后的周会  ";
    command.location = std::optional<std::string>{"会议室 B"};
    command.notes = std::optional<std::string>{};
    command.reminder_id = std::optional<int64_t>{42};
    command.status = ScheduleStatus::kComplete;

    const auto result = service.update_schedule(command);
    Check(result.status.ok() && result.schedule.has_value(), "合法字段修改应成功并返回完整日程");
    Check(result.schedule->event == "更新后的周会", "修改日程应清理事件名称两端空白");
    Check(result.schedule->location == "会议室 B" && !result.schedule->notes.has_value(),
          "修改日程应支持设置和清空可空文本字段");
    Check(result.schedule->reminder_id == 42 && result.schedule->status == ScheduleStatus::kComplete,
          "修改日程应支持关联提醒和完成状态");
}

/**
 * @brief 校验时间字段清空和时间范围约束。
 * @param service 被测试的日程服务。
 * @return 无返回值。
 */
void CheckTimeUpdates(ScheduleService& service) {
    UpdateScheduleCommand clear_time;
    clear_time.schedule_id = 1001;
    clear_time.start_time = std::optional<DateTime>{};
    clear_time.end_time = std::optional<DateTime>{};
    Check(service.update_schedule(clear_time).status.ok(), "同时清空开始和结束时间应成功");

    UpdateScheduleCommand invalid_range;
    invalid_range.schedule_id = 1001;
    invalid_range.start_time = std::optional<DateTime>{At(1'800'004'000)};
    invalid_range.end_time = std::optional<DateTime>{At(1'800'003'000)};
    Check(service.update_schedule(invalid_range).status.code == ErrorCode::kInvalidArgument,
          "修改后的结束时间不晚于开始时间时应拒绝修改");
}

/**
 * @brief 校验修改日程的冲突检测和忽略冲突语义。
 * @param service 被测试的日程服务。
 * @return 无返回值。
 */
void CheckConflicts(ScheduleService& service) {
    UpdateScheduleCommand conflict;
    conflict.schedule_id = 1001;
    conflict.start_time = std::optional<DateTime>{At(1'800'007'200)};
    conflict.end_time = std::optional<DateTime>{};

    const auto conflict_result = service.update_schedule(conflict);
    Check(conflict_result.status.code == ErrorCode::kConflict && !conflict_result.schedule.has_value(),
          "未忽略冲突时应只返回冲突且不返回修改后的日程");
    Check(conflict_result.conflicts.size() == 1 && conflict_result.conflicts.front().id == 1002,
          "冲突结果应包含目标日程之外的冲突日程");

    conflict.ignore_conflict = true;
    const auto ignored_result = service.update_schedule(conflict);
    Check(ignored_result.status.ok() && ignored_result.schedule.has_value() && ignored_result.conflicts.size() == 1,
          "忽略冲突时应完成修改并保留冲突列表");
}

/**
 * @brief 校验修改日程的错误输入。
 * @param service 被测试的日程服务。
 * @return 无返回值。
 */
void CheckInvalidInputs(ScheduleService& service) {
    UpdateScheduleCommand missing;
    missing.schedule_id = 9999;
    missing.event = "不存在";
    Check(service.update_schedule(missing).status.code == ErrorCode::kNotFound, "不存在的日程 ID 应返回未找到");

    UpdateScheduleCommand no_fields;
    no_fields.schedule_id = 1001;
    Check(service.update_schedule(no_fields).status.code == ErrorCode::kInvalidArgument,
          "未提供任何修改字段时应拒绝调用");

    UpdateScheduleCommand empty_event;
    empty_event.schedule_id = 1001;
    empty_event.event = "   ";
    Check(service.update_schedule(empty_event).status.code == ErrorCode::kInvalidArgument,
          "事件名称不得通过修改被清空");
}

}  // namespace

int main() {
    ScheduleService service;
    CheckFieldUpdates(service);
    CheckTimeUpdates(service);
    CheckConflicts(service);
    CheckInvalidInputs(service);
    return 0;
}
