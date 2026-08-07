#include <array>
#include <barrier>
#include <chrono>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "../src/mock/schedule_mock_data.h"
#include "../src/mock/schedule_operation_mock_data.h"
#include "support/test_support.h"
#include "voicelife/schedule/schedule_service.h"

using voicelife::ErrorCode;
using voicelife::schedule::AppendMockScheduleOperationForTesting;
using voicelife::schedule::DateTime;
using voicelife::schedule::FindMockScheduleById;
using voicelife::schedule::FindUndoableMockScheduleOperation;
using voicelife::schedule::LoadMockScheduleOperations;
using voicelife::schedule::OperationRecord;
using voicelife::schedule::RecordScheduleOperationCommand;
using voicelife::schedule::ResetMockScheduleOperationsForTesting;
using voicelife::schedule::Schedule;
using voicelife::schedule::ScheduleOperationType;
using voicelife::schedule::ScheduleService;
using voicelife::schedule::ScheduleStatus;
using voicelife::schedule::SeedMockSchedulesForTesting;
using voicelife::schedule::UndoScheduleOperationCommand;
using voicelife::test::Check;

namespace {

/**
 * @brief 返回当前秒级系统时间。
 * @return 当前日程时间。
 */
DateTime Now() { return std::chrono::time_point_cast<std::chrono::seconds>(std::chrono::system_clock::now()); }

/**
 * @brief 构造字段完整且便于比较的测试日程。
 * @param id 日程 ID。
 * @param event 日程名称。
 * @param status 日程状态。
 * @return 包含固定时间和可选字段的日程。
 */
Schedule MakeSchedule(int64_t id, std::string event, ScheduleStatus status = ScheduleStatus::kActive) {
    return {
        .id = id,
        .event = std::move(event),
        .start_time = DateTime{std::chrono::seconds{1'900'000'000 + id}},
        .end_time = DateTime{std::chrono::seconds{1'900'003'600 + id}},
        .location = "会议室 " + std::to_string(id),
        .notes = "日程备注 " + std::to_string(id),
        .reminder_id = 10'000 + id,
        .status = status,
        .created_at = DateTime{std::chrono::seconds{1'899'000'000 + id}},
        .updated_at = DateTime{std::chrono::seconds{1'899'100'000 + id}},
    };
}

/**
 * @brief 比较两个日程的全部领域字段。
 * @param left 左侧日程。
 * @param right 右侧日程。
 * @return 全部字段一致时返回 true。
 */
bool SameSchedule(const Schedule& left, const Schedule& right) {
    return left.id == right.id && left.event == right.event && left.start_time == right.start_time &&
           left.end_time == right.end_time && left.location == right.location && left.notes == right.notes &&
           left.reminder_id == right.reminder_id && left.status == right.status &&
           left.created_at == right.created_at && left.updated_at == right.updated_at;
}

/**
 * @brief 清空操作记录并用指定日程替换模拟存储。
 * @param schedules 本场景的初始日程集合。
 * @return 无返回值。
 */
void ResetScenario(std::vector<Schedule> schedules) {
    ResetMockScheduleOperationsForTesting();
    SeedMockSchedulesForTesting(std::move(schedules));
}

/**
 * @brief 通过服务记录一条测试操作并断言写入成功。
 * @param service 被测试的日程服务。
 * @param type 操作类型。
 * @param schedule_id 日程 ID。
 * @param event 日程名称。
 * @param previous 操作前的日程状态。
 * @return 保存后带有 ID 和时间的操作记录。
 */
OperationRecord RecordOperation(ScheduleService& service, ScheduleOperationType type, int64_t schedule_id,
                                std::string event, std::optional<Schedule> previous) {
    const auto result = service.record_schedule_operation({
        .type = type,
        .schedule_id = schedule_id,
        .schedule_event = std::move(event),
        .previous = std::move(previous),
    });
    Check(result.status.ok() && result.operation.has_value(), "测试操作记录应写入成功");
    return *result.operation;
}

/**
 * @brief 验证参数错误和不存在的操作不会改变任何状态。
 * @param service 被测试的日程服务。
 * @return 无返回值；断言失败时终止测试。
 */
void CheckInvalidAndMissingOperation(ScheduleService& service) {
    ResetScenario({});

    const auto invalid = service.undo_schedule_operation({.operation_id = 0});
    Check(invalid.status.code == ErrorCode::kInvalidArgument && !invalid.undone && !invalid.operation.has_value() &&
              !invalid.schedule.has_value() && !invalid.error.empty(),
          "非正数操作 ID 应返回完整的参数错误结果");

    const auto missing = service.undo_schedule_operation({.operation_id = 9999});
    Check(missing.status.code == ErrorCode::kNotFound && !missing.undone && !missing.operation.has_value() &&
              !missing.schedule.has_value() && !missing.error.empty(),
          "不存在的操作应返回未找到且不携带实体");
    Check(LoadMockScheduleOperations().empty(), "失败的撤销不应生成 undo 记录");
}

/**
 * @brief 验证撤销创建会删除日程，并可通过撤销 undo 恢复和再次删除。
 * @param service 被测试的日程服务。
 * @return 无返回值；断言失败时终止测试。
 */
void CheckCreateAndRecursiveUndo(ScheduleService& service) {
    const Schedule created = MakeSchedule(7001, "新建项目周会");
    ResetScenario({created});
    const OperationRecord create =
        RecordOperation(service, ScheduleOperationType::kCreate, created.id, created.event, std::nullopt);

    const auto first = service.undo_schedule_operation({.operation_id = create.id});
    Check(first.status.ok() && first.undone && first.operation.has_value() && first.operation->id == create.id &&
              first.operation->type == ScheduleOperationType::kCreate && !first.schedule.has_value() &&
              first.error.empty(),
          "撤销创建应返回原创建操作并删除日程");
    Check(!FindMockScheduleById(created.id).ok(), "撤销创建后模拟存储不应保留日程");

    const auto after_first = service.query_recent_schedule_operation();
    Check(after_first.operations.size() == 1 && after_first.operations.front().type == ScheduleOperationType::kUndo &&
              after_first.operations.front().previous.has_value() &&
              SameSchedule(*after_first.operations.front().previous, created),
          "成功撤销应隐藏原操作并记录携带撤销前快照的 undo");

    const auto repeated = service.undo_schedule_operation({.operation_id = create.id});
    Check(repeated.status.code == ErrorCode::kNotFound && !repeated.undone, "原操作成功撤销后不应允许按原 ID 重复撤销");

    const auto second = service.undo_schedule_operation({.operation_id = after_first.operations.front().id});
    Check(second.status.ok() && second.undone && second.operation.has_value() &&
              second.operation->type == ScheduleOperationType::kUndo && second.schedule.has_value() &&
              SameSchedule(*second.schedule, created),
          "撤销刚才的 undo 应恢复被删除的日程");

    const auto after_second = service.query_recent_schedule_operation();
    Check(after_second.operations.size() == 1 && after_second.operations.front().type == ScheduleOperationType::kUndo &&
              !after_second.operations.front().previous.has_value(),
          "恢复日程产生的新 undo 应用空快照表达撤销前日程不存在");

    const auto third = service.undo_schedule_operation({.operation_id = after_second.operations.front().id});
    Check(third.status.ok() && third.undone && !third.schedule.has_value() && !FindMockScheduleById(created.id).ok(),
          "空快照 undo 被撤销时应再次删除日程");
}

/**
 * @brief 验证撤销修改会完整恢复 previous，并记录撤销前的修改后状态。
 * @param service 被测试的日程服务。
 * @return 无返回值；断言失败时终止测试。
 */
void CheckUpdateUndo(ScheduleService& service) {
    const Schedule previous = MakeSchedule(7101, "修改前");
    Schedule updated = MakeSchedule(7101, "修改后", ScheduleStatus::kCompleted);
    updated.location = std::nullopt;
    updated.notes = "修改后的备注";
    ResetScenario({updated});
    const OperationRecord operation =
        RecordOperation(service, ScheduleOperationType::kUpdate, updated.id, updated.event, previous);

    const auto result = service.undo_schedule_operation({.operation_id = operation.id});
    const auto stored = FindMockScheduleById(previous.id);
    Check(result.status.ok() && result.undone && result.schedule.has_value() &&
              SameSchedule(*result.schedule, previous) && stored.ok() && SameSchedule(*stored.value, previous),
          "撤销修改应完整恢复 previous 的全部字段");

    const auto recent = service.query_recent_schedule_operation();
    Check(recent.operations.size() == 1 && recent.operations.front().type == ScheduleOperationType::kUndo &&
              recent.operations.front().previous.has_value() &&
              SameSchedule(*recent.operations.front().previous, updated),
          "修改撤销记录应保存撤销前的修改后状态");
}

/**
 * @brief 验证撤销删除会恢复日程，并可通过撤销 undo 回到已取消状态。
 * @param service 被测试的日程服务。
 * @return 无返回值；断言失败时终止测试。
 */
void CheckDeleteUndo(ScheduleService& service) {
    const Schedule previous = MakeSchedule(7201, "被删除的日程");
    Schedule cancelled = previous;
    cancelled.status = ScheduleStatus::kCancelled;
    cancelled.updated_at += std::chrono::minutes{1};
    ResetScenario({cancelled});
    const OperationRecord operation =
        RecordOperation(service, ScheduleOperationType::kDelete, previous.id, previous.event, previous);

    const auto restored = service.undo_schedule_operation({.operation_id = operation.id});
    Check(restored.status.ok() && restored.undone && restored.schedule.has_value() &&
              SameSchedule(*restored.schedule, previous),
          "撤销删除应恢复删除前的完整日程");

    const auto undo_records = service.query_recent_schedule_operation().operations;
    Check(undo_records.size() == 1 && undo_records.front().previous.has_value() &&
              SameSchedule(*undo_records.front().previous, cancelled),
          "删除撤销产生的 undo 应保存已取消状态");

    const auto reverted = service.undo_schedule_operation({.operation_id = undo_records.front().id});
    Check(reverted.status.ok() && reverted.schedule.has_value() && SameSchedule(*reverted.schedule, cancelled),
          "撤销删除对应的 undo 应恢复已取消状态");
}

/**
 * @brief 验证过期操作和日程逆操作失败都不会消费目标记录。
 * @param service 被测试的日程服务。
 * @return 无返回值；断言失败时终止测试。
 */
void CheckFailureDoesNotConsumeOperation(ScheduleService& service) {
    const Schedule expired_schedule = MakeSchedule(7301, "过期日程");
    ResetScenario({expired_schedule});
    const DateTime now = Now();
    const auto expired = AppendMockScheduleOperationForTesting(
        OperationRecord{
            .id = 0,
            .type = ScheduleOperationType::kCreate,
            .schedule_id = expired_schedule.id,
            .schedule_event = expired_schedule.event,
            .operated_at = {},
            .previous = std::nullopt,
        },
        now - std::chrono::minutes{16});
    Check(expired.ok(), "过期场景操作记录应成功注入");

    const auto expired_result = service.undo_schedule_operation({.operation_id = expired.value->id});
    Check(expired_result.status.code == ErrorCode::kConflict && !expired_result.undone &&
              FindMockScheduleById(expired_schedule.id).ok() && LoadMockScheduleOperations().size() == 1,
          "过期操作应保持日程和原操作记录不变");

    ResetScenario({});
    const OperationRecord missing_schedule =
        RecordOperation(service, ScheduleOperationType::kCreate, 7302, "不存在的已创建日程", std::nullopt);
    const auto failed = service.undo_schedule_operation({.operation_id = missing_schedule.id});
    Check(failed.status.code == ErrorCode::kNotFound && !failed.undone && LoadMockScheduleOperations().size() == 1 &&
              LoadMockScheduleOperations().front().id == missing_schedule.id,
          "日程逆操作失败时不应失效目标或写入 undo 记录");
}

/**
 * @brief 验证十五分钟闭区间边界与未来操作判断。
 * @return 无返回值；断言失败时终止测试。
 */
void CheckUndoWindowBoundary() {
    ResetScenario({});
    const DateTime now = Now();
    const auto boundary = AppendMockScheduleOperationForTesting(
        OperationRecord{
            .id = 0,
            .type = ScheduleOperationType::kCreate,
            .schedule_id = 7401,
            .schedule_event = "窗口边界",
            .operated_at = {},
            .previous = std::nullopt,
        },
        now - std::chrono::minutes{15});
    Check(boundary.ok() && FindUndoableMockScheduleOperation(boundary.value->id, now).ok(),
          "恰好十五分钟前的操作应仍可撤销");

    const auto future = AppendMockScheduleOperationForTesting(
        OperationRecord{
            .id = 0,
            .type = ScheduleOperationType::kCreate,
            .schedule_id = 7402,
            .schedule_event = "未来操作",
            .operated_at = {},
            .previous = std::nullopt,
        },
        now + std::chrono::seconds{1});
    Check(future.ok() && FindUndoableMockScheduleOperation(future.value->id, now).status.code == ErrorCode::kConflict,
          "晚于当前时间的操作不应允许撤销");
}

/**
 * @brief 验证并发撤销同一操作时只有一个请求成功。
 * @param service 被测试的日程服务。
 * @return 无返回值；断言失败时终止测试。
 */
void CheckConcurrentUndo(ScheduleService& service) {
    for (int iteration = 0; iteration < 20; ++iteration) {
        const Schedule previous = MakeSchedule(7500 + iteration, "并发撤销前");
        Schedule updated = previous;
        updated.event = "并发撤销后";
        ResetScenario({updated});
        const OperationRecord operation =
            RecordOperation(service, ScheduleOperationType::kUpdate, updated.id, updated.event, previous);

        std::barrier start(3);
        std::array<voicelife::schedule::UndoScheduleOperationResult, 2> results;
        std::thread first([&] {
            start.arrive_and_wait();
            results[0] = service.undo_schedule_operation({.operation_id = operation.id});
        });
        std::thread second([&] {
            start.arrive_and_wait();
            results[1] = service.undo_schedule_operation({.operation_id = operation.id});
        });
        start.arrive_and_wait();
        first.join();
        second.join();

        const int successes = static_cast<int>(results[0].undone) + static_cast<int>(results[1].undone);
        const auto stored = FindMockScheduleById(previous.id);
        const auto recent = service.query_recent_schedule_operation();
        Check(successes == 1 && stored.ok() && SameSchedule(*stored.value, previous) && recent.operations.size() == 1 &&
                  recent.operations.front().type == ScheduleOperationType::kUndo,
              "并发撤销同一操作应恰好成功一次并保留一条一致的 undo 记录");
    }
}

}  // namespace

/**
 * @brief 执行日程操作撤销服务测试。
 * @return 全部断言通过时返回 0。
 */
int main() {
    ScheduleService service;
    CheckInvalidAndMissingOperation(service);
    CheckCreateAndRecursiveUndo(service);
    CheckUpdateUndo(service);
    CheckDeleteUndo(service);
    CheckFailureDoesNotConsumeOperation(service);
    CheckUndoWindowBoundary();
    CheckConcurrentUndo(service);
    return 0;
}
