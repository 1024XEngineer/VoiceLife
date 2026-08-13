#include <chrono>

#include "support/in_memory_schedule_repository.h"
#include "support/test_support.h"
#include "voicelife/schedule/schedule_service.h"

using voicelife::ErrorCode;
using voicelife::schedule::DateTime;
using voicelife::schedule::QueryScheduleCommand;
using voicelife::schedule::ScheduleService;
using voicelife::schedule::ScheduleStatusFilter;
using voicelife::test::Check;
using voicelife::test::InMemoryScheduleRepository;

namespace {

/** @brief 将测试 Unix 秒转换为日程时间。 @param seconds Unix 秒。 @return 日程时间。 */
DateTime At(int64_t seconds) { return DateTime{std::chrono::seconds{seconds}}; }

/** @brief 验证默认状态、排序和无开始时间日程的查询行为。 @param service 日程服务。 @return 无。 */
void CheckDefaultQuery(const ScheduleService& service) {
    const auto result = service.query_schedule({});
    Check(result.status.ok() && result.total == 2 && result.schedules.size() == 2, "默认应查询全部有效日程");
    Check(result.schedules[0].id == 2001 && result.schedules[1].id == 2004, "结果应按开始时间升序且无时间日程在后");
}

/** @brief 验证多个筛选条件按 AND 关系生效。 @param service 日程服务。 @return 无。 */
void CheckCombinedFilters(const ScheduleService& service) {
    QueryScheduleCommand command;
    command.keyword = "数据库 +连接";
    command.start_from = At(1'810'000'000);
    command.start_to = At(1'810'007'200);
    command.status = ScheduleStatusFilter::kAll;
    const auto result = service.query_schedule(command);
    Check(result.status.ok() && result.total == 2, "关键词拆分和包含边界的时间范围应共同生效");
    Check(result.schedules[0].id == 2001 && result.schedules[1].id == 2002, "匹配结果应按开始时间升序返回");

    command.schedule_id = 2002;
    command.status = ScheduleStatusFilter::kCompleted;
    Check(service.query_schedule(command).total == 1, "ID、关键词、时间和完成状态应按 AND 精确过滤");
}

/** @brief 验证状态筛选、分页和分页前总数。 @param service 日程服务。 @return 无。 */
void CheckStatusAndPagination(const ScheduleService& service) {
    QueryScheduleCommand cancelled;
    cancelled.status = ScheduleStatusFilter::kCancelled;
    Check(service.query_schedule(cancelled).schedules[0].id == 2003, "应支持查询已取消日程");

    QueryScheduleCommand page;
    page.status = ScheduleStatusFilter::kAll;
    page.limit = 2;
    page.offset = 1;
    const auto result = service.query_schedule(page);
    Check(result.total == 4 && result.schedules.size() == 2, "total 应为分页前数量，结果应应用分页参数");
    Check(result.schedules[0].id == 2003 && result.schedules[1].id == 2002, "分页应在排序后应用");

    page.offset = INT64_MAX;
    Check(service.query_schedule(page).schedules.empty(), "超大分页偏移量应稳定返回空页");
}

/** @brief 验证非法查询参数会返回明确错误。 @param service 日程服务。 @return 无。 */
void CheckValidation(const ScheduleService& service) {
    QueryScheduleCommand invalid_id;
    invalid_id.schedule_id = 0;
    Check(service.query_schedule(invalid_id).status.code == ErrorCode::kInvalidArgument, "零日程 ID 应被拒绝");

    QueryScheduleCommand invalid_range;
    invalid_range.start_from = At(20);
    invalid_range.start_to = At(10);
    Check(service.query_schedule(invalid_range).status.code == ErrorCode::kInvalidArgument, "反向时间范围应被拒绝");

    QueryScheduleCommand invalid_limit;
    invalid_limit.limit = 51;
    Check(service.query_schedule(invalid_limit).status.code == ErrorCode::kInvalidArgument, "超过最大返回条数应被拒绝");

    invalid_limit.limit = 0;
    Check(service.query_schedule(invalid_limit).status.code == ErrorCode::kInvalidArgument, "零返回条数应被拒绝");

    QueryScheduleCommand invalid_offset;
    invalid_offset.offset = -1;
    Check(!service.query_schedule(invalid_offset).error.empty(), "负分页偏移量应返回错误信息");
}

/** @brief 验证关键词的大小写归一化和空加号词处理。 @param service 日程服务。 @return 无。 */
void CheckKeywordNormalization(const ScheduleService& service) {
    QueryScheduleCommand case_insensitive;
    case_insensitive.keyword = "数据库 +连接";
    case_insensitive.status = ScheduleStatusFilter::kAll;
    Check(service.query_schedule(case_insensitive).total == 2, "多个关键词应同时匹配事件标题");

    QueryScheduleCommand empty_required_token;
    empty_required_token.keyword = "+   ";
    Check(service.query_schedule(empty_required_token).total == 2, "空加号词不应过滤有效日程");
}

}  // namespace

int main() {
    InMemoryScheduleRepository repository(InMemoryScheduleRepository::QuerySchedules());
    const ScheduleService service(repository, repository);
    CheckDefaultQuery(service);
    CheckCombinedFilters(service);
    CheckStatusAndPagination(service);
    CheckValidation(service);
    CheckKeywordNormalization(service);
    return 0;
}
