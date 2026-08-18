#include <optional>
#include <string>

#include "support/in_memory_schedule_repository.h"
#include "support/test_support.h"
#include "voicelife/schedule/schedule_operation_service.h"

using voicelife::ErrorCode;
using voicelife::schedule::DateTime;
using voicelife::schedule::OperationRecord;
using voicelife::schedule::RecordScheduleOperationCommand;
using voicelife::schedule::Schedule;
using voicelife::schedule::ScheduleOperationService;
using voicelife::schedule::ScheduleOperationType;
using voicelife::test::Check;
using voicelife::test::InMemoryScheduleRepository;

namespace {

/**
 * @brief 验证记录成功时会生成 ID 和操作时间，并保留操作字段。
 * @param service 被测试的日程操作服务。
 * @return 无返回值；断言失败时终止测试。
 */
void CheckSuccessfulRecord(ScheduleOperationService& service) {
    RecordScheduleOperationCommand command{
        .type = ScheduleOperationType::kCreate,
        .schedule_id = 3001,
        .schedule_event = "  新日程  ",
        .previous = std::nullopt,
    };
    const auto result = service.record_schedule_operation(command);
    Check(result.result.ok() && result.result.value.has_value() && result.result.error.empty(), "创建操作记录应成功");
    Check(result.result.value->id > 0 && result.result.value->operated_at != DateTime{}, "操作 ID 和时间应由系统生成");
    Check(result.result.value->type == ScheduleOperationType::kCreate && result.result.value->schedule_id == 3001 &&
              result.result.value->schedule_event == "新日程" && !result.result.value->previous.has_value(),
          "操作记录应保留规范化后的创建信息");
}

/**
 * @brief 验证修改和删除操作必须携带操作前快照。
 * @param service 被测试的日程操作服务。
 * @return 无返回值；断言失败时终止测试。
 */
void CheckPreviousStateRules(ScheduleOperationService& service) {
    const Schedule previous{
        .id = 3004,
        .event = "删除前",
        .start_time = std::nullopt,
        .end_time = std::nullopt,
        .location = "会议室 A",
        .notes = std::nullopt,
        .rule_id = std::nullopt,
        .status = voicelife::schedule::ScheduleStatus::kActive,
        .created_at = DateTime{},
        .updated_at = DateTime{},
    };
    RecordScheduleOperationCommand missing_previous{
        .type = ScheduleOperationType::kUpdate,
        .schedule_id = 3002,
        .schedule_event = "修改日程",
        .previous = std::nullopt,
    };
    Check(service.record_schedule_operation(missing_previous).result.status.code == ErrorCode::kInvalidArgument,
          "修改操作缺少 previous 时应拒绝");

    RecordScheduleOperationCommand create_with_previous{
        .type = ScheduleOperationType::kCreate,
        .schedule_id = 3003,
        .schedule_event = "创建日程",
        .previous = previous,
    };
    Check(service.record_schedule_operation(create_with_previous).result.status.code == ErrorCode::kInvalidArgument,
          "创建操作携带 previous 时应拒绝");

    RecordScheduleOperationCommand deleted{
        .type = ScheduleOperationType::kDelete,
        .schedule_id = 3004,
        .schedule_event = "删除日程",
        .previous = previous,
    };
    const auto result = service.record_schedule_operation(deleted);
    Check(result.result.ok() && result.result.value->type == ScheduleOperationType::kDelete &&
              result.result.value->previous->id == previous.id &&
              result.result.value->previous->event == previous.event &&
              result.result.value->previous->location == previous.location,
          "删除操作应保留删除前快照");

    RecordScheduleOperationCommand undo_without_previous{
        .type = ScheduleOperationType::kUndo,
        .schedule_id = 3005,
        .schedule_event = "撤销创建",
        .previous = std::nullopt,
    };
    Check(service.record_schedule_operation(undo_without_previous).result.ok(),
          "撤销前日程不存在时 undo 操作应允许 previous 为空");

    RecordScheduleOperationCommand mismatched_previous{
        .type = ScheduleOperationType::kUpdate,
        .schedule_id = 3006,
        .schedule_event = "快照错配",
        .previous = previous,
    };
    Check(service.record_schedule_operation(mismatched_previous).result.status.code == ErrorCode::kInvalidArgument,
          "操作记录应拒绝与日程 ID 不一致的 previous 快照");
}

/**
 * @brief 验证记录命令的类型、标识和标题校验。
 * @param service 被测试的日程操作服务。
 * @return 无返回值；断言失败时终止测试。
 */
void CheckInvalidArguments(ScheduleOperationService& service) {
    RecordScheduleOperationCommand invalid_type{
        .type = static_cast<ScheduleOperationType>(99),
        .schedule_id = 3005,
        .schedule_event = "非法类型",
        .previous = std::nullopt,
    };
    Check(service.record_schedule_operation(invalid_type).result.status.code == ErrorCode::kInvalidArgument,
          "未知操作类型应拒绝");

    RecordScheduleOperationCommand invalid_id{
        .type = ScheduleOperationType::kCreate,
        .schedule_id = 0,
        .schedule_event = "非法 ID",
        .previous = std::nullopt,
    };
    Check(service.record_schedule_operation(invalid_id).result.status.code == ErrorCode::kInvalidArgument,
          "非正数日程 ID 应拒绝");

    RecordScheduleOperationCommand empty_event{
        .type = ScheduleOperationType::kCreate,
        .schedule_id = 3006,
        .schedule_event = " \t\n ",
        .previous = std::nullopt,
    };
    const auto empty_result = service.record_schedule_operation(empty_event);
    Check(empty_result.result.status.code == ErrorCode::kInvalidArgument && !empty_result.result.value.has_value() &&
              !empty_result.result.error.empty(),
          "空标题应返回带错误信息的参数错误");

    RecordScheduleOperationCommand too_long{
        .type = ScheduleOperationType::kCreate,
        .schedule_id = 3007,
        .schedule_event = std::string(101, 'a'),
        .previous = std::nullopt,
    };
    Check(service.record_schedule_operation(too_long).result.status.code == ErrorCode::kInvalidArgument,
          "超过标题长度上限应拒绝");
}

/**
 * @brief 验证操作仓储不会按记录条数裁剪操作。
 * @param service 被测试的日程操作服务。
 * @param repository 被测试的内存操作仓储。
 * @return 无返回值；断言失败时终止测试。
 */
void CheckOperationStorageHasNoCountLimit(ScheduleOperationService& service, InMemoryScheduleRepository& repository) {
    const std::size_t original_count = repository.ActiveOperations().size();
    OperationRecord latest;
    for (int index = 0; index < 11; ++index) {
        RecordScheduleOperationCommand command{
            .type = ScheduleOperationType::kCreate,
            .schedule_id = 4000 + index,
            .schedule_event = "容量测试 " + std::to_string(index),
            .previous = std::nullopt,
        };
        const auto result = service.record_schedule_operation(command);
        Check(result.result.ok() && result.result.value.has_value(), "容量测试操作应成功");
        latest = *result.result.value;
    }

    const auto operations = repository.ActiveOperations();
    Check(operations.size() == original_count + 11, "操作仓储不应限制记录条数");
    Check(operations.back().id == latest.id && operations.back().schedule_id == latest.schedule_id,
          "操作仓储应保留最新记录");
    Check(operations[original_count].schedule_id == 4000, "超过十条时不应淘汰最早的操作记录");
}

}  // namespace

/**
 * @brief 执行日程操作记录服务测试。
 * @return 全部断言通过时返回 0。
 */
int main() {
    InMemoryScheduleRepository repository;
    ScheduleOperationService service(repository);
    CheckSuccessfulRecord(service);
    CheckPreviousStateRules(service);
    CheckInvalidArguments(service);
    CheckOperationStorageHasNoCountLimit(service, repository);

    // 仓储失败路径：记录操作写入失败时应透传底层错误。
    {
        InMemoryScheduleRepository failure_repository;
        ScheduleOperationService failure_service(failure_repository);
        failure_repository.FailNextInsertOperation(voicelife::Status::Error(ErrorCode::kInternal, "操作写入失败"));
        RecordScheduleOperationCommand command{
            .type = ScheduleOperationType::kCreate,
            .schedule_id = 9001,
            .schedule_event = "写入失败",
            .previous = std::nullopt,
        };
        Check(failure_service.record_schedule_operation(command).result.status.code == ErrorCode::kInternal,
              "record 应透传 InsertOperation 错误");
    }
    return 0;
}
