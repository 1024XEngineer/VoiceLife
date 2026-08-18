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
    int64_t cancelled_count = 0;
    const Status cancelled = rule_repository.CancelRuleAndInstances(rule.rule->id, cancelled_count);
    Check(cancelled.ok() && cancelled_count == 1 && rule_repository.FindByRule(rule.rule->id).value->empty(),
          "取消规则必须同时取消实例并清理例外");

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
    return 0;
}
