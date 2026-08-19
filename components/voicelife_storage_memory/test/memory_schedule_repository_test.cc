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
    Schedule missing_schedule{};
    missing_schedule.id = 9999;
    Check(!repository.Update(missing_schedule).ok(), "更新不存在的日程必须失败");

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
    Check(!repository.Insert(Schedule{}).ok() && !repository.FindById(9999).ok(), "空日程和不存在的日程必须被拒绝");
    const auto active_only = repository.Find({.schedule_id = std::nullopt,
                                              .rule_id = std::nullopt,
                                              .keyword = "+下午 会议",
                                              .start_from = std::nullopt,
                                              .start_to = std::nullopt,
                                              .status = ScheduleStatusFilter::kActive,
                                              .limit = 10,
                                              .offset = 0});
    Check(active_only.ok() && active_only.value->empty(), "关键词和状态筛选必须同时生效");
    const auto completed_only = repository.Find({.schedule_id = updated.id,
                                                 .rule_id = std::nullopt,
                                                 .keyword = "更新 内存",
                                                 .start_from = std::nullopt,
                                                 .start_to = std::nullopt,
                                                 .status = ScheduleStatusFilter::kCompleted,
                                                 .limit = 10,
                                                 .offset = 0});
    Check(completed_only.ok() && completed_only.value->size() == 1, "标识、关键词和完成状态筛选必须命中同一日程");
    Check(repository
              .Find({.schedule_id = std::nullopt,
                     .rule_id = 9999,
                     .keyword = std::nullopt,
                     .start_from = std::nullopt,
                     .start_to = std::nullopt,
                     .status = ScheduleStatusFilter::kAll,
                     .limit = 10,
                     .offset = 0})
              .value->empty(),
          "规则筛选必须排除没有对应规则标识的日程");
    Check(repository
              .Find({.schedule_id = std::nullopt,
                     .rule_id = std::nullopt,
                     .keyword = "不存在",
                     .start_from = DateTime{std::chrono::seconds{2'050'000'000}},
                     .start_to = DateTime{std::chrono::seconds{2'050'000'000}},
                     .status = ScheduleStatusFilter::kAll,
                     .limit = 10,
                     .offset = 100})
              .value->empty(),
          "时间窗、未命中关键词和越界分页必须返回空集合");
    Check(repository.FindAll().value->size() == 3, "全量读取必须保留已取消和已完成的日程");
    const auto excluded_overlap =
        repository.FindOverlapping(DateTime{std::chrono::seconds{2'050'000'600}},
                                   DateTime{std::chrono::seconds{2'050'001'200}}, timed_insert.value->id);
    Check(excluded_overlap.ok() && excluded_overlap.value->empty(), "重叠查询必须支持排除自身");

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
    ScheduleRule direct_rule;
    direct_rule.event = "直接规则";
    const auto inserted_rule = rule_repository.Insert(direct_rule);
    Check(inserted_rule.ok() && rule_repository.FindAll().value->size() == 2 &&
              rule_repository.FindById(inserted_rule.value->id).ok() && !rule_repository.FindById(9999).ok(),
          "规则仓储必须支持插入、枚举和按标识读取");
    Check(!rule_repository.Insert(ScheduleRule{}).ok(), "空规则必须被拒绝");
    ScheduleRule invalid_update = *inserted_rule.value;
    invalid_update.event.clear();
    Check(!rule_repository.Update(invalid_update).ok(), "空规则更新必须被拒绝");
    Check(!rule_repository.CreateWithFirstInstance(ScheduleRule{}, std::nullopt).ok(), "原子创建必须拒绝空规则");
    Schedule invalid_first_instance;
    Check(!rule_repository.CreateWithFirstInstance(*inserted_rule.value, invalid_first_instance).ok(),
          "原子创建必须拒绝空首条实例");

    ScheduleException exception;
    exception.rule_id = rule.rule->id;
    exception.original_start_time = DateTime{std::chrono::seconds{2'000'000'000}};
    exception.type = ExceptionType::kSkip;
    const auto first_exception = rule_repository.Upsert(exception);
    const auto second_exception = rule_repository.Upsert(exception);
    Check(first_exception.ok() && second_exception.ok() && first_exception.value->id == second_exception.value->id,
          "内存存储必须按规则和时间幂等 upsert 例外");
    Check(!rule_repository.Upsert(ScheduleException{}).ok(), "无效例外规则标识必须被拒绝");
    const auto found_exception = rule_repository.FindByRuleAndTime(rule.rule->id, exception.original_start_time);
    const auto missing_exception =
        rule_repository.FindByRuleAndTime(rule.rule->id, DateTime{std::chrono::seconds{2'000'000'001}});
    Check(found_exception.ok() && found_exception.value->has_value() && missing_exception.ok() &&
              !missing_exception.value->has_value(),
          "例外必须支持按规则和发生时间读取");

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
    ScheduleRule missing_rule{};
    missing_rule.id = 9999;
    Check(!rule_repository.Update(missing_rule).ok(), "不存在的规则必须拒绝更新");
    Check(!rule_repository.UpdateAndRebuild(missing_rule, std::nullopt).ok(), "重建不存在的规则必须失败");
    Schedule invalid_next_instance;
    Check(!rule_repository.CreateNextInstance(invalid_next_instance, std::nullopt).ok(),
          "下一实例必须包含名称和规则标识");
    Schedule next_instance;
    next_instance.event = "下一个实例";
    next_instance.rule_id = inserted_rule.value->id;
    next_instance.start_time = DateTime{std::chrono::seconds{2'200'000'000}};
    ScheduleException linked_exception;
    linked_exception.rule_id = inserted_rule.value->id;
    linked_exception.original_start_time = *next_instance.start_time;
    const auto next_created = rule_repository.CreateNextInstance(next_instance, linked_exception);
    const auto linked_saved = rule_repository.FindByRuleAndTime(inserted_rule.value->id, *next_instance.start_time);
    Check(next_created.ok() && linked_saved.ok() && linked_saved.value->has_value() &&
              linked_saved.value->value().schedule_id == next_created.value->id,
          "下一实例必须回写关联例外的日程标识");

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
    const auto second_operation = repository.InsertOperation({.entity_type = OperationEntityType::kRule,
                                                              .type = ScheduleOperationType::kUpdate,
                                                              .entity_id = inserted_rule.value->id,
                                                              .operated_at = DateTime{},
                                                              .label = "Direct Rule",
                                                              .before = "{}"});
    Check(second_operation.ok() && repository
                                           .FindOperations({.operation_id = std::nullopt,
                                                            .entity_type = OperationEntityType::kRule,
                                                            .entity_id = inserted_rule.value->id,
                                                            .type = ScheduleOperationType::kUpdate,
                                                            .operated_from = std::nullopt,
                                                            .operated_to = std::nullopt,
                                                            .keyword = "+direct rule",
                                                            .limit = 10,
                                                            .offset = 0})
                                           .value->size() == 1,
          "操作查询必须同时支持类型、实体和多词关键词筛选");
    Check(repository
              .FindOperations({.operation_id = std::nullopt,
                               .entity_type = OperationEntityType::kException,
                               .entity_id = std::nullopt,
                               .type = std::nullopt,
                               .operated_from = std::nullopt,
                               .operated_to = std::nullopt,
                               .keyword = std::nullopt,
                               .limit = 10,
                               .offset = 0})
              .value->empty(),
          "未命中的操作筛选必须返回空集合");
    return 0;
}
