#include <optional>
#include <string>

#include "support/in_memory_schedule_repository.h"
#include "support/test_support.h"
#include "voicelife/schedule/schedule_operation_service.h"

using voicelife::ErrorCode;
using voicelife::schedule::DateTime;
using voicelife::schedule::OperationEntityType;
using voicelife::schedule::OperationRecord;
using voicelife::schedule::RecordOperationCommand;
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
    RecordOperationCommand command{
        .entity_type = OperationEntityType::kSchedule,
        .type = ScheduleOperationType::kCreate,
        .entity_id = 3001,
        .label = "新日程",
        .before = std::nullopt,
    };
    const auto result = service.record_operation(command);
    Check(result.result.ok() && result.result.value.has_value() && result.result.error.empty(), "创建操作记录应成功");
    Check(result.result.value->id > 0 && result.result.value->operated_at != DateTime{}, "操作 ID 和时间应由系统生成");
    Check(result.result.value->entity_type == OperationEntityType::kSchedule &&
              result.result.value->type == ScheduleOperationType::kCreate && result.result.value->entity_id == 3001 &&
              result.result.value->label == "新日程" && !result.result.value->before.has_value(),
          "操作记录应保留创建信息");
}

/**
 * @brief 验证修改和删除操作必须携带操作前快照。
 * @param service 被测试的日程操作服务。
 * @return 无返回值；断言失败时终止测试。
 */
void CheckPreviousStateRules(ScheduleOperationService& service) {
    const std::string snapshot = R"({"id":3004,"event":"删除前","location":"会议室 A"})";

    RecordOperationCommand missing_before{
        .entity_type = OperationEntityType::kSchedule,
        .type = ScheduleOperationType::kUpdate,
        .entity_id = 3002,
        .label = "修改日程",
        .before = std::nullopt,
    };
    Check(service.record_operation(missing_before).result.status.code == ErrorCode::kInvalidArgument,
          "修改操作缺少 before 时应拒绝");

    RecordOperationCommand create_with_before{
        .entity_type = OperationEntityType::kSchedule,
        .type = ScheduleOperationType::kCreate,
        .entity_id = 3003,
        .label = "创建日程",
        .before = snapshot,
    };
    Check(service.record_operation(create_with_before).result.status.code == ErrorCode::kInvalidArgument,
          "创建操作携带 before 时应拒绝");

    RecordOperationCommand deleted{
        .entity_type = OperationEntityType::kSchedule,
        .type = ScheduleOperationType::kDelete,
        .entity_id = 3004,
        .label = "删除日程",
        .before = snapshot,
    };
    const auto result = service.record_operation(deleted);
    Check(result.result.ok() && result.result.value->type == ScheduleOperationType::kDelete &&
              result.result.value->before == snapshot,
          "删除操作应保留删除前快照");
}

/**
 * @brief 验证记录命令的类型、标识和名称校验。
 * @param service 被测试的日程操作服务。
 * @return 无返回值；断言失败时终止测试。
 */
void CheckInvalidArguments(ScheduleOperationService& service) {
    RecordOperationCommand invalid_entity{
        .entity_type = static_cast<OperationEntityType>(99),
        .type = ScheduleOperationType::kCreate,
        .entity_id = 3005,
        .label = "非法实体类型",
        .before = std::nullopt,
    };
    Check(service.record_operation(invalid_entity).result.status.code == ErrorCode::kInvalidArgument,
          "未知实体类型应拒绝");

    RecordOperationCommand invalid_type{
        .entity_type = OperationEntityType::kSchedule,
        .type = static_cast<ScheduleOperationType>(99),
        .entity_id = 3006,
        .label = "非法操作类型",
        .before = std::nullopt,
    };
    Check(service.record_operation(invalid_type).result.status.code == ErrorCode::kInvalidArgument,
          "未知操作类型应拒绝");

    RecordOperationCommand invalid_id{
        .entity_type = OperationEntityType::kSchedule,
        .type = ScheduleOperationType::kCreate,
        .entity_id = 0,
        .label = "非法 ID",
        .before = std::nullopt,
    };
    Check(service.record_operation(invalid_id).result.status.code == ErrorCode::kInvalidArgument,
          "非正数实体 ID 应拒绝");

    RecordOperationCommand empty_label{
        .entity_type = OperationEntityType::kSchedule,
        .type = ScheduleOperationType::kCreate,
        .entity_id = 3007,
        .label = " \t\n ",
        .before = std::nullopt,
    };
    const auto empty_result = service.record_operation(empty_label);
    Check(empty_result.result.status.code == ErrorCode::kInvalidArgument && !empty_result.result.value.has_value() &&
              !empty_result.result.error.empty(),
          "空名称应返回带错误信息的参数错误");

    RecordOperationCommand too_long{
        .entity_type = OperationEntityType::kSchedule,
        .type = ScheduleOperationType::kCreate,
        .entity_id = 3008,
        .label = std::string(101, 'a'),
        .before = std::nullopt,
    };
    Check(service.record_operation(too_long).result.status.code == ErrorCode::kInvalidArgument,
          "超过名称长度上限应拒绝");

    RecordOperationCommand oversized_before{
        .entity_type = OperationEntityType::kSchedule,
        .type = ScheduleOperationType::kDelete,
        .entity_id = 3009,
        .label = "快照过大",
        .before = std::string(2049, 'x'),
    };
    Check(service.record_operation(oversized_before).result.status.code == ErrorCode::kInvalidArgument,
          "超过快照字节上限应拒绝");
}

/**
 * @brief 验证操作仓储不会按记录条数裁剪操作。
 * @param service 被测试的日程操作服务。
 * @param repository 被测试的内存操作仓储。
 * @return 无返回值；断言失败时终止测试。
 */
void CheckOperationStorageHasNoCountLimit(ScheduleOperationService& service, InMemoryScheduleRepository& repository) {
    const std::size_t original_count = repository.Operations().size();
    OperationRecord latest;
    for (int index = 0; index < 11; ++index) {
        RecordOperationCommand command{
            .entity_type = OperationEntityType::kSchedule,
            .type = ScheduleOperationType::kCreate,
            .entity_id = 4000 + index,
            .label = "容量测试 " + std::to_string(index),
            .before = std::nullopt,
        };
        const auto result = service.record_operation(command);
        Check(result.result.ok() && result.result.value.has_value(), "容量测试操作应成功");
        latest = *result.result.value;
    }

    const auto operations = repository.Operations();
    Check(operations.size() == original_count + 11, "操作仓储不应限制记录条数");
    Check(operations.back().id == latest.id && operations.back().entity_id == latest.entity_id,
          "操作仓储应保留最新记录");
    Check(operations[original_count].entity_id == 4000, "超过十条时不应淘汰最早的操作记录");
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
        RecordOperationCommand command{
            .entity_type = OperationEntityType::kSchedule,
            .type = ScheduleOperationType::kCreate,
            .entity_id = 9001,
            .label = "写入失败",
            .before = std::nullopt,
        };
        Check(failure_service.record_operation(command).result.status.code == ErrorCode::kInternal,
              "record_operation 应透传 InsertOperation 错误");
    }
    return 0;
}
