#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include "support/in_memory_schedule_repository.h"
#include "support/test_support.h"
#include "voicelife/schedule/schedule_operation_service.h"

using voicelife::ErrorCode;
using voicelife::schedule::DateTime;
using voicelife::schedule::OperationEntityType;
using voicelife::schedule::OperationRecord;
using voicelife::schedule::QueryOperationCommand;
using voicelife::schedule::ScheduleOperationService;
using voicelife::schedule::ScheduleOperationType;
using voicelife::test::Check;
using voicelife::test::InMemoryScheduleRepository;

namespace {

/** @brief 将 Unix 秒转换为日程时间。 @param unix_seconds Unix 秒。 @return 对应的日程时间。 */
DateTime At(int64_t unix_seconds) { return DateTime{std::chrono::seconds{unix_seconds}}; }

/**
 * @brief 构造操作记录供仓储直接插入。
 * @param operated_at 操作时间。
 * @param entity_type 实体类型。
 * @param type 操作类型。
 * @param entity_id 实体标识。
 * @param label 展示名称。
 * @return 操作记录；before 按操作类型补齐或留空。
 */
OperationRecord MakeRecord(DateTime operated_at, OperationEntityType entity_type, ScheduleOperationType type,
                           int64_t entity_id, std::string label) {
    return {
        .id = 0,
        .entity_type = entity_type,
        .type = type,
        .entity_id = entity_id,
        .operated_at = operated_at,
        .label = std::move(label),
        .before = type == ScheduleOperationType::kCreate
                      ? std::nullopt
                      : std::optional<std::string>(std::string(R"({"id":)") + std::to_string(entity_id) + "}"),
    };
}

/** @brief 验证无操作时返回成功的空结果。 @param service 被测试服务。 @return 无。 */
void CheckEmptyQuery(ScheduleOperationService& service) {
    const auto result = service.query_operations({});
    Check(result.result.ok() && result.result.value.empty() && result.total == 0 && result.result.error.empty(),
          "无操作时应返回成功的空结果");
}

/**
 * @brief 验证筛选、排序、分页和总数统计。
 * @param service 被测试服务。
 * @param repository 内存仓储，用于按指定时间写入操作。
 * @return 无。
 */
void CheckQueryFilters(ScheduleOperationService& service, InMemoryScheduleRepository& repository) {
    // 插入顺序决定操作 ID：晨会=1、评审=2、周期规则=3、晨会改期=4。
    repository.InsertOperationAt(
        MakeRecord(At(100), OperationEntityType::kSchedule, ScheduleOperationType::kCreate, 1001, "晨会"), At(100));
    repository.InsertOperationAt(
        MakeRecord(At(200), OperationEntityType::kSchedule, ScheduleOperationType::kCreate, 1002, "评审"), At(200));
    repository.InsertOperationAt(
        MakeRecord(At(300), OperationEntityType::kRule, ScheduleOperationType::kCreate, 2001, "周期规则"), At(300));
    repository.InsertOperationAt(
        MakeRecord(At(400), OperationEntityType::kSchedule, ScheduleOperationType::kUpdate, 1001, "晨会改期"), At(400));

    // 无筛选默认分页：全部命中并按时间倒序。
    const auto all = service.query_operations({});
    Check(all.result.ok() && all.result.value.size() == 4 && all.total == 4, "默认查询应返回全部操作");
    Check(all.result.value.front().label == "晨会改期" && all.result.value.back().label == "晨会",
          "默认查询应按时间倒序返回");

    // 实体类型筛选。
    QueryOperationCommand rule_only;
    rule_only.entity_type = OperationEntityType::kRule;
    const auto rules = service.query_operations(rule_only);
    Check(rules.result.ok() && rules.result.value.size() == 1 && rules.total == 1 &&
              rules.result.value.front().label == "周期规则",
          "实体类型筛选应命中规则操作");

    // 实体标识需配合实体类型。
    QueryOperationCommand by_entity;
    by_entity.entity_type = OperationEntityType::kSchedule;
    by_entity.entity_id = int64_t{1001};
    const auto entity_result = service.query_operations(by_entity);
    Check(entity_result.result.ok() && entity_result.result.value.size() == 2 && entity_result.total == 2,
          "按实体标识查询应命中该日程的两条操作");

    // 操作类型筛选。
    QueryOperationCommand create_only;
    create_only.type = ScheduleOperationType::kCreate;
    const auto creates = service.query_operations(create_only);
    Check(creates.result.ok() && creates.result.value.size() == 3 && creates.total == 3,
          "按操作类型查询应命中创建操作");

    // 名称模糊匹配。
    QueryOperationCommand keyword;
    keyword.keyword = std::string{"晨会"};
    const auto by_keyword = service.query_operations(keyword);
    Check(by_keyword.result.ok() && by_keyword.result.value.size() == 2 && by_keyword.total == 2,
          "按名称模糊查询应命中两条晨会操作");

    // 时间窗口闭区间。
    QueryOperationCommand window;
    window.operated_from = At(150);
    window.operated_to = At(350);
    const auto ranged = service.query_operations(window);
    Check(ranged.result.ok() && ranged.result.value.size() == 2 && ranged.total == 2, "时间窗口应命中窗口内操作");

    // 分页裁剪结果但不影响总数。
    QueryOperationCommand paged;
    paged.limit = 2;
    paged.offset = 1;
    const auto page = service.query_operations(paged);
    Check(page.result.ok() && page.result.value.size() == 2 && page.total == 4, "分页应裁剪结果但保留总数");
    Check(page.result.value.front().label == "周期规则" && page.result.value.back().label == "评审",
          "分页应按倒序返回正确切片");

    // 按操作 ID 精确查询。
    QueryOperationCommand by_operation_id;
    by_operation_id.operation_id = int64_t{2};
    const auto precise = service.query_operations(by_operation_id);
    Check(precise.result.ok() && precise.result.value.size() == 1 && precise.total == 1 &&
              precise.result.value.front().label == "评审",
          "按操作 ID 应返回单条记录");

    // 重复查询不应消费记录。
    const auto again = service.query_operations({});
    Check(again.result.ok() && again.result.value.size() == 4, "重复查询不应消费操作记录");
}

/** @brief 验证查询命令的参数校验。 @param service 被测试服务。 @return 无。 */
void CheckInvalidQueryArguments(ScheduleOperationService& service) {
    QueryOperationCommand missing_entity_type;
    missing_entity_type.entity_id = int64_t{100};
    Check(service.query_operations(missing_entity_type).result.status.code == ErrorCode::kInvalidArgument,
          "按实体 ID 查询时必须提供实体类型");

    QueryOperationCommand bad_entity;
    bad_entity.entity_type = static_cast<OperationEntityType>(99);
    Check(service.query_operations(bad_entity).result.status.code == ErrorCode::kInvalidArgument, "未知实体类型应拒绝");

    QueryOperationCommand bad_type;
    bad_type.type = static_cast<ScheduleOperationType>(99);
    Check(service.query_operations(bad_type).result.status.code == ErrorCode::kInvalidArgument, "未知操作类型应拒绝");

    QueryOperationCommand reversed;
    reversed.operated_from = At(200);
    reversed.operated_to = At(100);
    Check(service.query_operations(reversed).result.status.code == ErrorCode::kInvalidArgument,
          "时间下界晚于上界应拒绝");

    QueryOperationCommand zero_limit;
    zero_limit.limit = 0;
    Check(service.query_operations(zero_limit).result.status.code == ErrorCode::kInvalidArgument,
          "limit 小于 1 应拒绝");

    QueryOperationCommand big_limit;
    big_limit.limit = 101;
    Check(service.query_operations(big_limit).result.status.code == ErrorCode::kInvalidArgument,
          "limit 超过 100 应拒绝");

    QueryOperationCommand negative_offset;
    negative_offset.offset = -1;
    Check(service.query_operations(negative_offset).result.status.code == ErrorCode::kInvalidArgument,
          "offset 小于 0 应拒绝");
}

}  // namespace

/** @brief 执行操作记录查询测试。 @return 全部断言通过时返回 0。 */
int main() {
    InMemoryScheduleRepository repository;
    ScheduleOperationService service(repository);
    CheckEmptyQuery(service);
    CheckQueryFilters(service, repository);
    CheckInvalidQueryArguments(service);

    // 仓储失败路径：查询或计数失败时应透传底层错误。
    {
        InMemoryScheduleRepository failure_repository;
        ScheduleOperationService failure_service(failure_repository);
        failure_repository.FailNextFindOperations(voicelife::Status::Error(ErrorCode::kUnavailable, "操作查询失败"));
        Check(failure_service.query_operations({}).result.status.code == ErrorCode::kUnavailable,
              "query_operations 应透传 FindOperations 错误");
    }
    {
        InMemoryScheduleRepository failure_repository;
        ScheduleOperationService failure_service(failure_repository);
        failure_repository.FailNextCountOperations(voicelife::Status::Error(ErrorCode::kUnavailable, "操作计数失败"));
        Check(failure_service.query_operations({}).result.status.code == ErrorCode::kUnavailable,
              "query_operations 应透传 CountOperations 错误");
    }
    return 0;
}
