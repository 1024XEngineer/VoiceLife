#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include "schedule_operation_query_helpers.h"
#include "support/test_support.h"
#include "voicelife/schedule/schedule_service.h"

using voicelife::schedule::DateTime;
using voicelife::schedule::FilterRecentScheduleOperations;
using voicelife::schedule::OperationRecord;
using voicelife::schedule::RecordScheduleOperationCommand;
using voicelife::schedule::ScheduleOperationType;
using voicelife::schedule::ScheduleService;
using voicelife::test::Check;

namespace {

/** @brief 将 Unix 秒转换为日程时间。 @param unix_seconds Unix 秒。 @return 对应的日程时间。 */
DateTime At(int64_t unix_seconds) { return DateTime{std::chrono::seconds{unix_seconds}}; }

/**
 * @brief 构造用于最近操作筛选测试的操作记录。
 * @param id 操作记录 ID。
 * @param operated_at 操作时间。
 * @return 包含固定日程字段的操作记录。
 */
OperationRecord MakeOperation(int64_t id, DateTime operated_at) {
    return {
        .id = id,
        .type = ScheduleOperationType::kCreate,
        .schedule_id = 5000 + id,
        .schedule_event = "查询测试 " + std::to_string(id),
        .operated_at = operated_at,
        .previous = std::nullopt,
    };
}

/** @brief 验证十五分钟窗口边界和倒序规则。 @return 无返回值；断言失败时终止测试。 */
void CheckWindowAndOrdering() {
    const DateTime now = At(10'000);
    std::vector<OperationRecord> operations{
        MakeOperation(1, now - std::chrono::minutes{15}),
        MakeOperation(2, now - std::chrono::minutes{15} - std::chrono::seconds{1}),
        MakeOperation(3, now - std::chrono::minutes{1}),
        MakeOperation(4, now + std::chrono::seconds{1}),
        MakeOperation(5, now),
        MakeOperation(6, now),
    };

    const auto result = FilterRecentScheduleOperations(std::move(operations), now);
    Check(result.size() == 4, "查询应包含十五分钟整边界并排除过期和未来操作");
    Check(result[0].id == 6 && result[1].id == 5 && result[2].id == 3 && result[3].id == 1,
          "最近操作应按时间倒序排列，同秒时按操作 ID 倒序排列");
}

/**
 * @brief 验证服务返回十五分钟内全部操作且查询不会消费记录。
 * @param service 被测试的日程服务。
 * @return 无返回值；断言失败时终止测试。
 */
void CheckServiceQuery(ScheduleService& service) {
    const auto empty = service.query_recent_schedule_operation();
    Check(empty.status.ok() && empty.operations.empty() && empty.error.empty(), "无操作时应返回成功的空结果");

    for (int index = 0; index < 12; ++index) {
        RecordScheduleOperationCommand command{
            .type = ScheduleOperationType::kCreate,
            .schedule_id = 6000 + index,
            .schedule_event = "最近操作 " + std::to_string(index),
            .previous = std::nullopt,
        };
        Check(service.record_schedule_operation(command).status.ok(), "查询测试的操作记录应写入成功");
    }

    const auto first = service.query_recent_schedule_operation();
    const auto second = service.query_recent_schedule_operation();
    Check(first.status.ok() && first.error.empty() && first.operations.size() == 12,
          "查询应返回十五分钟内全部操作且不限制为十条");
    Check(first.operations.front().schedule_id == 6011 && first.operations.back().schedule_id == 6000,
          "服务结果应按最新操作优先排列");
    Check(second.operations.size() == first.operations.size() &&
              second.operations.front().id == first.operations.front().id,
          "重复查询不应消费操作记录");
}

}  // namespace

/** @brief 执行最近日程操作查询测试。 @return 全部断言通过时返回 0。 */
int main() {
    CheckWindowAndOrdering();
    ScheduleService service;
    CheckServiceQuery(service);
    return 0;
}
