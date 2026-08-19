#include "voicelife/storage_memory/memory_schedule_repository.h"

#include <chrono>
#include <cstdlib>
#include <iostream>

#include "voicelife/schedule/schedule_operation_service.h"
#include "voicelife/schedule/schedule_rule_service.h"
#include "voicelife/schedule/schedule_service.h"

namespace {

void Check(bool condition, const char* message) {
    if (condition) return;
    std::cerr << "FAILED: " << message << '\n';
    std::exit(1);
}

}  // namespace

int main() {
    using namespace voicelife;
    using namespace voicelife::schedule;
    storage_memory::MemoryScheduleRepository repository;
    ScheduleOperationService operations(repository);
    ScheduleService schedules(repository, &operations);
    storage_memory::MemoryScheduleRuleRepository rule_repository(repository);
    ScheduleRuleService rules(rule_repository, rule_repository, repository);

    const auto created = schedules.create_schedule({.event = "内存日程",
                                                    .start_time = std::nullopt,
                                                    .end_time = std::nullopt,
                                                    .location = std::nullopt,
                                                    .notes = std::nullopt,
                                                    .ignore_conflict = false});
    Check(created.result.ok() && created.result.value.has_value(), "内存存储必须创建日程");
    const auto queried = schedules.query_schedule({.schedule_id = created.result.value->id,
                                                   .rule_id = std::nullopt,
                                                   .keyword = std::nullopt,
                                                   .start_from = std::nullopt,
                                                   .start_to = std::nullopt,
                                                   .status = ScheduleStatusFilter::kAll,
                                                   .limit = 10,
                                                   .offset = 0});
    Check(queried.result.ok() && queried.total == 1, "内存存储必须查询刚创建的日程");

    Schedule updated = *created.result.value;
    updated.event = "已更新的内存日程";
    updated.status = ScheduleStatus::kCompleted;
    Check(repository.Update(updated).ok(), "内存存储必须更新已有日程");
    const auto updated_schedule = repository.FindById(updated.id);
    Check(updated_schedule.ok() && updated_schedule.value->event == updated.event &&
              updated_schedule.value->status == ScheduleStatus::kCompleted,
          "更新必须保留新的名称和状态");
    Check(!repository.Update(Schedule{.id = 9999}).ok(), "更新不存在的日程必须失败");

    Schedule timed_schedule;
    timed_schedule.event = "下午会议";
    timed_schedule.start_time = DateTime{std::chrono::seconds{2'050'000'000}};
    timed_schedule.end_time = DateTime{std::chrono::seconds{2'050'001'800}};
    const auto timed_insert = repository.Insert(timed_schedule);
    Check(timed_insert.ok(), "内存存储必须创建带时间的日程");
    Schedule later_schedule;
    later_schedule.event = "下午复盘";
    later_schedule.start_time = DateTime{std::chrono::seconds{2'050'002'000}};
    const auto later_insert = repository.Insert(later_schedule);
    Check(later_insert.ok(), "内存存储必须创建第二条带时间日程");
    const auto keyword_query = repository.Find({.schedule_id = std::nullopt,
                                                .rule_id = std::nullopt,
                                                .keyword = "下午",
                                                .start_from = DateTime{std::chrono::seconds{2'050'000'000}},
                                                .start_to = DateTime{std::chrono::seconds{2'050'003'000}},
                                                .status = ScheduleStatusFilter::kAll,
                                                .limit = 1,
                                                .offset = 1});
    Check(keyword_query.ok() && keyword_query.value->size() == 1 &&
              keyword_query.value->front().id == later_insert.value->id,
          "查询必须按时间排序并支持关键词、时间窗和分页");
    const auto overlaps = repository.FindOverlapping(DateTime{std::chrono::seconds{2'050'000'600}},
                                                     DateTime{std::chrono::seconds{2'050'001'200}}, std::nullopt);
    Check(overlaps.ok() && overlaps.value->size() == 1 && overlaps.value->front().id == timed_insert.value->id,
          "重叠查询必须排除无时间和非活动日程");
    Check(repository.Delete(timed_insert.value->id).ok(), "删除日程必须标记为已取消");
    Check(!repository.Delete(timed_insert.value->id).ok() && !repository.Delete(9999).ok(),
          "重复删除和删除不存在的日程必须失败");
    Check(repository
                  .Count({.schedule_id = std::nullopt,
                          .rule_id = std::nullopt,
                          .keyword = "下午",
                          .start_from = std::nullopt,
                          .start_to = std::nullopt,
                          .status = ScheduleStatusFilter::kAll,
                          .limit = 10,
                          .offset = 0})
                  .value == 2,
          "计数必须使用与查询相同的筛选条件");

    const auto rule = rules.create_schedule_rule({.event = "每日内存规则",
                                                  .freq_type = Frequency::kDaily,
                                                  .start_time = LocalTime{9, 0, 0},
                                                  .start_date = LocalDate{2099, 1, 1},
                                                  .end_time = std::nullopt,
                                                  .location = std::nullopt,
                                                  .notes = std::nullopt,
                                                  .interval_val = 1,
                                                  .weekdays_mask = std::nullopt,
                                                  .day_of_month = std::nullopt,
                                                  .month_of_year = std::nullopt,
                                                  .monthly_mode = std::nullopt,
                                                  .end_date = std::nullopt,
                                                  .occurrence_count = std::nullopt,
                                                  .ignore_conflict = false});
    Check(rule.status.ok() && rule.rule.has_value() && rule.schedules.size() == 1,
          "内存存储必须原子创建规则和首条实例");

    ScheduleException exception;
    exception.rule_id = rule.rule->id;
    exception.original_start_time = DateTime{std::chrono::seconds{2'000'000'000}};
    exception.type = ExceptionType::kSkip;
    const auto first_exception = rule_repository.Upsert(exception);
    const auto second_exception = rule_repository.Upsert(exception);
    Check(first_exception.ok() && second_exception.ok() && first_exception.value->id == second_exception.value->id,
          "内存存储必须按规则和时间幂等 upsert 例外");

    Schedule rebuilt_instance;
    rebuilt_instance.event = "重建后的实例";
    rebuilt_instance.start_time = DateTime{std::chrono::seconds{2'100'000'000}};
    ScheduleRule rebuilt_rule = *rule.rule;
    rebuilt_rule.event = "重建后的规则";
    const auto rebuilt = rule_repository.UpdateAndRebuild(rebuilt_rule, rebuilt_instance);
    Check(rebuilt.ok() && rebuilt.value->event == "重建后的规则", "内存存储必须原子更新规则并重建实例");
    Check(rule_repository.DeleteFuture(rule.rule->id, DateTime{std::chrono::seconds{1'900'000'000}}).ok() &&
              rule_repository.FindByRule(rule.rule->id).value->empty(),
          "删除未来例外必须只影响指定规则的例外");
    int64_t cancelled_count = 0;
    const Status cancelled = rule_repository.CancelRuleAndInstances(rule.rule->id, cancelled_count);
    Check(cancelled.ok() && cancelled_count == 1 && rule_repository.FindByRule(rule.rule->id).value->empty(),
          "取消规则必须同时取消实例并清理例外");
    Check(!rule_repository.Update(ScheduleRule{.id = 9999}).ok(), "不存在的规则必须拒绝更新");

    const auto operation = operations.record_operation({.entity_type = OperationEntityType::kSchedule,
                                                        .type = ScheduleOperationType::kCreate,
                                                        .entity_id = created.result.value->id,
                                                        .label = "内存日程",
                                                        .before = std::nullopt});
    Check(operation.result.ok() && operation.result.value.has_value(), "内存存储必须记录操作");
    const auto operation_query = repository.FindOperations({.operation_id = operation.result.value->id,
                                                            .entity_type = OperationEntityType::kSchedule,
                                                            .entity_id = created.result.value->id,
                                                            .type = ScheduleOperationType::kCreate,
                                                            .operated_from = std::nullopt,
                                                            .operated_to = std::nullopt,
                                                            .keyword = "内存",
                                                            .limit = 10,
                                                            .offset = 0});
    Check(operation_query.ok() && operation_query.value->size() == 1, "内存存储必须匹配操作标识和关键词筛选");
    const auto all_operations = repository.FindOperations({.operation_id = std::nullopt,
                                                           .entity_type = std::nullopt,
                                                           .entity_id = std::nullopt,
                                                           .type = std::nullopt,
                                                           .operated_from = std::nullopt,
                                                           .operated_to = std::nullopt,
                                                           .keyword = std::nullopt,
                                                           .limit = 1,
                                                           .offset = 0});
    Check(all_operations.ok() && all_operations.value->size() == 1 &&
              repository
                      .CountOperations({.operation_id = std::nullopt,
                                        .entity_type = std::nullopt,
                                        .entity_id = std::nullopt,
                                        .type = std::nullopt,
                                        .operated_from = std::nullopt,
                                        .operated_to = std::nullopt,
                                        .keyword = std::nullopt,
                                        .limit = 10,
                                        .offset = 0})
                      .value >= 2,
          "操作记录必须支持分页和计数");
    return 0;
}
