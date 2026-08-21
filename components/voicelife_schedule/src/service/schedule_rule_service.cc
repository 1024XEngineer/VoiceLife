#include "voicelife/schedule/schedule_rule_service.h"

#include <cstddef>
#include <utility>

#include "../helpers/schedule_create_helpers.h"
#include "../helpers/schedule_occurrence_helpers.h"
#include "../helpers/schedule_rule_result_helpers.h"
#include "../helpers/schedule_rule_update_helpers.h"
#include "../rules/recurrence_planner.h"
#include "../rules/schedule_time_rules.h"
#include "schedule_rule_service_helpers.h"
#include "voicelife/schedule/calendar.h"
#include "voicelife/schedule/schedule_factory.h"

namespace voicelife::schedule {
namespace {

using schedule_rule_service_helpers::AtLocalDate;
using schedule_rule_service_helpers::MatchesKeyword;
using schedule_rule_service_helpers::MatchesStatus;
using schedule_rule_service_helpers::NextOccurrences;
using schedule_rule_service_helpers::Now;
using schedule_rule_service_helpers::ValidateRuleDateRange;
using schedule_rule_service_helpers::ValidateRuleFields;

std::vector<DateTime> OccurrencesForQuery(const ScheduleRule& rule, DateTime now,
                                          const QueryScheduleRulesCommand& command) {
    DateTime from = command.occurrence_start.value_or(now);
    if (!command.occurrence_end.has_value()) return NextOccurrences(rule, from, 3);
    if (*command.occurrence_end <= from) return {};

    // 有明确日期窗口时不能用固定的“未来三条”预览，否则远期的单日查询会漏掉目标 occurrence。
    std::vector<DateTime> occurrences;
    DateTime cursor = from;
    constexpr int kMaximumQueryOccurrences = 256;
    for (int index = 0; index < kMaximumQueryOccurrences; ++index) {
        const std::optional<DateTime> next = NextOccurrence(rule, cursor);
        if (!next.has_value() || *next >= *command.occurrence_end) break;
        occurrences.push_back(*next);
        cursor = *next + std::chrono::seconds{1};
    }
    return occurrences;
}

}  // namespace

ScheduleRuleService::ScheduleRuleService(ScheduleRuleRepository& rule_repository,
                                         ScheduleExceptionRepository& exception_repository,
                                         ScheduleRepository& schedule_repository)
    : rule_repository_(rule_repository),
      exception_repository_(exception_repository),
      schedule_repository_(schedule_repository) {}

CreateScheduleRuleResult ScheduleRuleService::create_schedule_rule(const CreateScheduleRuleCommand& command) const {
    // 从命令组装规则领域实体，再统一做规则参数校验。
    ScheduleRule rule = ScheduleFactory::CreateRuleFromCommand(command);
    const DateTime now = Now();
    const Status field_validation = ValidateRuleFields(rule);
    if (!field_validation.ok()) {
        return FailedCreateScheduleRuleResult(field_validation);
    }

    // 如果调用方指定了开始日期，则从该日期开始计算；否则从当前日期开始计算。
    rule.start_date = command.start_date.value_or(LocalDateFromUtc(now));
    const DateTime search_from =
        command.start_date.has_value() ? AtLocalDate(*command.start_date, rule.start_time) : now;
    const std::optional<DateTime> first_time = NextOccurrence(rule, search_from);
    if (!first_time.has_value()) {
        return FailedCreateScheduleRuleResult(
            Status::Error(ErrorCode::kInvalidArgument, "无法根据当前周期规则计算首个发生时间"));
    }
    // 首条实例的开始时间就是规则的持久化 start_date。
    rule.start_date = LocalDateFromUtc(*first_time);

    const Status date_validation = ValidateRuleDateRange(rule);
    if (!date_validation.ok()) {
        return FailedCreateScheduleRuleResult(date_validation);
    }

    // 生成规则首条实例，作为创建时默认物化的日程数据。
    std::optional<Schedule> first_instance;
    first_instance = ScheduleFactory::CreateOccurrence(rule, *first_time);

    // 搜集首条实例附近候选日程，做冲突检测和提前返回。
    std::vector<Schedule> conflicts;
    if (first_instance.has_value() && first_instance->start_time.has_value()) {
        const auto [window_start, window_end] = ScheduleNearbyWindow(*first_instance);
        const Result<std::vector<Schedule>> candidates =
            schedule_repository_.FindOverlapping(window_start, window_end, std::nullopt);
        if (!candidates.ok()) {
            return FailedCreateScheduleRuleResult(
                Status::Error(candidates.status.code, "读取现有日程失败：" + candidates.status.message));
        }
        conflicts = FindConflictingSchedules(*first_instance, *candidates.value);
        if (!conflicts.empty() && !command.ignore_conflict) {
            return FailedCreateScheduleRuleResult(Status::Error(ErrorCode::kConflict, "首条实例与已有日程冲突"),
                                                  std::move(conflicts));
        }
    }

    // 由仓储在事务内同时创建规则和首条实例，保证规则与实例一致性。
    const Result<ScheduleRule> created = rule_repository_.CreateWithFirstInstance(rule, first_instance);
    if (!created.ok()) {
        return FailedCreateScheduleRuleResult(created.status, std::move(conflicts));
    }

    // 返回数据：首条实例补上仓储生成的 rule_id 后再随规则一起返回。
    std::vector<Schedule> schedules;
    if (first_instance.has_value()) {
        first_instance->rule_id = created.value->id;
        schedules.push_back(*first_instance);
    }
    return {.status = Status::Ok(),
            .rule = created.value,
            .schedules = std::move(schedules),
            .conflicts = std::move(conflicts),
            .error = {}};
}

QueryScheduleRulesResult ScheduleRuleService::query_schedule_rules(const QueryScheduleRulesCommand& command) const {
    // 读取规则集合，服务层负责规则筛选，并补齐每个规则的未来发生时间和例外。
    const Result<std::vector<ScheduleRule>> loaded = rule_repository_.FindAll();
    if (!loaded.ok()) {
        return FailedQueryScheduleRulesResult(loaded.status);
    }

    const DateTime now = Now();
    std::vector<ScheduleRuleView> views;
    for (const ScheduleRule& rule : *loaded.value) {
        if (command.rule_id.has_value() && rule.id != *command.rule_id) continue;
        if (command.keyword.has_value() && !MatchesKeyword(rule, *command.keyword)) continue;
        if (!MatchesStatus(rule, command.status)) continue;

        ScheduleRuleView view;
        view.rule = rule;
        view.upcoming_occurrences = OccurrencesForQuery(rule, now, command);
        const Result<std::vector<ScheduleException>> exceptions = exception_repository_.FindByRule(rule.id);
        if (!exceptions.ok()) {
            return FailedQueryScheduleRulesResult(exceptions.status);
        }
        view.exceptions = *exceptions.value;
        views.push_back(std::move(view));
    }

    // 完成分页截取，total 表示筛选后的完整结果数。
    const int64_t total = static_cast<int64_t>(views.size());
    const std::size_t begin = command.offset >= total ? views.size() : static_cast<std::size_t>(command.offset);
    const std::size_t count = std::min(static_cast<std::size_t>(command.limit), views.size() - begin);
    std::vector<ScheduleRuleView> page(views.begin() + static_cast<std::ptrdiff_t>(begin),
                                       views.begin() + static_cast<std::ptrdiff_t>(begin + count));
    return {.status = Status::Ok(), .rules = std::move(page), .total = total, .error = {}};
}

UpdateScheduleRuleResult ScheduleRuleService::update_schedule_rule(const UpdateScheduleRuleCommand& command) {
    // 先校验 ID，并读取当前规则快照作为合并基础。
    if (command.rule_id <= 0) {
        return FailedUpdateScheduleRuleResult(Status::Error(ErrorCode::kInvalidArgument, "规则 ID 必须大于零"));
    }
    const Result<ScheduleRule> loaded = rule_repository_.FindById(command.rule_id);
    if (!loaded.ok()) {
        return FailedUpdateScheduleRuleResult(loaded.status);
    }

    // 把本次提供的字段覆盖到旧规则上，未提供字段保持原值。
    ScheduleRule rule = *loaded.value;
    ApplyScheduleRulePatch(command, rule);

    const DateTime now = Now();
    const Status field_validation = ValidateRuleFields(rule);
    if (!field_validation.ok()) {
        return FailedUpdateScheduleRuleResult(field_validation);
    }

    // 更新会重建未来实例，因此先按新的开始日期重新计算下一个实例，再让首条实例开始时间回写 start_date。
    const DateTime search_from =
        command.start_date.has_value() ? AtLocalDate(*(*command.start_date), rule.start_time) : now;
    const std::optional<DateTime> first_time = NextOccurrence(rule, search_from);
    if (!first_time.has_value()) {
        return FailedUpdateScheduleRuleResult(
            Status::Error(ErrorCode::kInvalidArgument, "无法根据当前周期规则计算首个发生时间"));
    }
    rule.start_date = LocalDateFromUtc(*first_time);

    // 依赖 start_date 的约束放到新 start_date 计算完成后再校验。
    const Status date_validation = ValidateRuleDateRange(rule);
    if (!date_validation.ok()) {
        return FailedUpdateScheduleRuleResult(date_validation);
    }
    rule.updated_at = now;

    // 生成新规则下的首条实例，用于规则更新后立即重建下一个可见实例。
    std::optional<Schedule> first_instance;
    first_instance = ScheduleFactory::CreateOccurrence(rule, *first_time);

    // 对新建首条实例做冲突检测，并忽略该规则自身已有实例，避免误判为冲突。
    std::vector<Schedule> conflicts;
    if (first_instance.has_value() && first_instance->start_time.has_value()) {
        const auto [window_start, window_end] = ScheduleNearbyWindow(*first_instance);
        const Result<std::vector<Schedule>> candidates =
            schedule_repository_.FindOverlapping(window_start, window_end, std::nullopt);
        if (!candidates.ok()) {
            return FailedUpdateScheduleRuleResult(
                Status::Error(candidates.status.code, "读取现有日程失败：" + candidates.status.message));
        }
        conflicts = FindConflictingSchedules(*first_instance, *candidates.value, command.rule_id);
        if (!conflicts.empty() && !command.ignore_conflict) {
            return FailedUpdateScheduleRuleResult(Status::Error(ErrorCode::kConflict, "规则下一条实例与已有日程冲突"),
                                                  std::move(conflicts));
        }
    }

    // 由仓储事务完成规则更新、未来实例重建和例外清理。
    const Result<ScheduleRule> updated = rule_repository_.UpdateAndRebuild(rule, first_instance);
    if (!updated.ok()) {
        return FailedUpdateScheduleRuleResult(updated.status);
    }

    // 返回数据：用更新后的规则 ID 关联新首条实例，供调用方看到本次重建结果。
    std::vector<Schedule> schedules;
    if (first_instance.has_value()) {
        first_instance->rule_id = rule.id;
        schedules.push_back(*first_instance);
    }
    return {.status = Status::Ok(),
            .rule = updated.value,
            .schedules = std::move(schedules),
            .conflicts = std::move(conflicts),
            .error = {}};
}

CancelScheduleRuleResult ScheduleRuleService::cancel_schedule_rule(const CancelScheduleRuleCommand& command) {
    // 校验规则 ID。
    if (command.rule_id <= 0) {
        return FailedCancelScheduleRuleResult(Status::Error(ErrorCode::kInvalidArgument, "规则 ID 必须大于零"));
    }
    // 委托仓储在同一事务内取消规则、取消已落库实例并清理例外。
    int64_t cancelled_count = 0;
    const Status cancelled = rule_repository_.CancelRuleAndInstances(command.rule_id, cancelled_count);
    if (!cancelled.ok()) {
        return FailedCancelScheduleRuleResult(cancelled);
    }
    // 读取取消后的规则快照，保证返回体中的 rule 与当前存储状态一致。
    const Result<ScheduleRule> rule = rule_repository_.FindById(command.rule_id);
    if (!rule.ok()) {
        return FailedCancelScheduleRuleResult(rule.status, cancelled_count);
    }
    return {.status = Status::Ok(), .rule = rule.value, .cancelled_count = cancelled_count, .error = {}};
}

UpdateScheduleOccurrenceResult ScheduleRuleService::update_schedule_occurrence(
    const UpdateScheduleOccurrenceCommand& command) {
    // 校验规则 ID，并确认本次有实际修改字段。
    if (command.rule_id <= 0) {
        return FailedUpdateScheduleOccurrenceResult(Status::Error(ErrorCode::kInvalidArgument, "规则 ID 必须大于零"));
    }
    const Result<ScheduleRule> rule = rule_repository_.FindById(command.rule_id);
    if (!rule.ok()) {
        return FailedUpdateScheduleOccurrenceResult(rule.status);
    }
    const bool has_update = command.event.has_value() || command.start_time.has_value() ||
                            command.end_time.has_value() || command.location.has_value() || command.notes.has_value();
    if (!has_update) {
        return FailedUpdateScheduleOccurrenceResult(
            Status::Error(ErrorCode::kInvalidArgument, "至少需要提供一个要修改的字段"));
    }

    // 读取既有例外，若不存在则组装新的 modify 例外，若存在则在其上覆盖字段。
    ScheduleException exception;
    const Result<std::optional<ScheduleException>> existing =
        exception_repository_.FindByRuleAndTime(command.rule_id, command.original_start_time);
    if (!existing.ok()) {
        return FailedUpdateScheduleOccurrenceResult(existing.status);
    }
    exception = (*existing.value).value_or(ScheduleException{});
    if (exception.rule_id == 0) {
        exception.rule_id = command.rule_id;
        exception.original_start_time = command.original_start_time;
        exception.type = ExceptionType::kModify;
    }
    exception.type = ExceptionType::kModify;
    ApplyScheduleOccurrencePatch(command, exception);

    // 未落库实例才允许通过 exception 修改；已落库实例必须走一次性 update_schedule。
    const Result<std::optional<Schedule>> materialized = FindMaterializedScheduleOccurrence(
        schedule_repository_, command.rule_id, command.original_start_time, exception.schedule_id);
    if (!materialized.ok()) {
        return FailedUpdateScheduleOccurrenceResult(materialized.status);
    }
    if (materialized.value->has_value()) {
        return FailedUpdateScheduleOccurrenceResult(
            Status::Error(ErrorCode::kConflict, "该周期实例已生成，请使用 update_schedule 修改"));
    }

    // 写入 exception，后续生成实例时按该例外覆盖到 schedule。
    const Result<ScheduleException> upserted = exception_repository_.Upsert(exception);
    if (!upserted.ok()) {
        return FailedUpdateScheduleOccurrenceResult(upserted.status);
    }
    return {
        .status = Status::Ok(), .schedule = std::nullopt, .exception = upserted.value, .conflicts = {}, .error = {}};
}

SkipScheduleOccurrenceResult ScheduleRuleService::skip_schedule_occurrence(
    const SkipScheduleOccurrenceCommand& command) {
    // 校验规则 ID。
    if (command.rule_id <= 0) {
        return FailedSkipScheduleOccurrenceResult(Status::Error(ErrorCode::kInvalidArgument, "规则 ID 必须大于零"));
    }

    // 若已有例外则直接返回，跳过操作本身幂等。
    const Result<std::optional<ScheduleException>> existing =
        exception_repository_.FindByRuleAndTime(command.rule_id, command.original_start_time);
    if (!existing.ok()) {
        return FailedSkipScheduleOccurrenceResult(existing.status);
    }
    const std::optional<ScheduleException>& maybe_existing = *existing.value;
    if (maybe_existing.has_value()) {
        return {.status = Status::Ok(), .schedule = std::nullopt, .exception = maybe_existing, .error = {}};
    }

    // 已落库实例不能通过 occurrence 跳过，应让调用方走 cancel_schedule。
    const Result<std::optional<Schedule>> materialized = FindMaterializedScheduleOccurrence(
        schedule_repository_, command.rule_id, command.original_start_time, std::nullopt);
    if (!materialized.ok()) {
        return FailedSkipScheduleOccurrenceResult(materialized.status);
    }
    if (materialized.value->has_value()) {
        return FailedSkipScheduleOccurrenceResult(
            Status::Error(ErrorCode::kConflict, "该周期实例已生成，请使用 update_schedule 或 cancel_schedule 处理"));
    }

    // 组装 skip 例外，阻止后续生成该时间点的日程实例。
    ScheduleException exception;
    exception.rule_id = command.rule_id;
    exception.original_start_time = command.original_start_time;
    exception.type = ExceptionType::kSkip;

    const Result<ScheduleException> upserted = exception_repository_.Upsert(exception);
    if (!upserted.ok()) {
        return FailedSkipScheduleOccurrenceResult(upserted.status);
    }
    return {.status = Status::Ok(), .schedule = std::nullopt, .exception = upserted.value, .error = {}};
}

GenerateNextScheduleInstanceResult ScheduleRuleService::generate_next_schedule_instance(
    const GenerateNextScheduleInstanceCommand& command) {
    // 校验规则 ID，并读取规则定义。
    if (command.rule_id <= 0) {
        return FailedGenerateNextScheduleInstanceResult(
            Status::Error(ErrorCode::kInvalidArgument, "规则 ID 必须大于零"));
    }
    const Result<ScheduleRule> rule = rule_repository_.FindById(command.rule_id);
    if (!rule.ok()) {
        return FailedGenerateNextScheduleInstanceResult(rule.status);
    }

    // 从当前时间开始扫描候选发生时间，跳过已物化、已跳过或已带例外的点。
    DateTime cursor = Now();
    for (int attempt = 0; attempt < 1000; ++attempt) {
        const std::optional<DateTime> next = NextOccurrence(*rule.value, cursor);
        if (!next.has_value()) {
            return {.status = Status::Ok(), .schedule = std::nullopt, .error = {}};
        }

        const Result<std::optional<ScheduleException>> existing =
            exception_repository_.FindByRuleAndTime(command.rule_id, *next);
        if (!existing.ok()) {
            return FailedGenerateNextScheduleInstanceResult(existing.status);
        }
        const std::optional<ScheduleException>& maybe_exception = *existing.value;

        // 已物化或已跳过时，推进到下一个候选时间点继续查找。
        if (maybe_exception.has_value()) {
            if (maybe_exception->schedule_id.has_value()) {
                cursor = *next + std::chrono::seconds{1};
                continue;
            }
            if (maybe_exception->type == ExceptionType::kSkip) {
                cursor = *next + std::chrono::seconds{1};
                continue;
            }
        } else {
            const Result<std::optional<Schedule>> materialized =
                FindMaterializedScheduleOccurrence(schedule_repository_, command.rule_id, *next, std::nullopt);
            if (!materialized.ok()) {
                return FailedGenerateNextScheduleInstanceResult(materialized.status);
            }
            if (materialized.value->has_value()) {
                cursor = *next + std::chrono::seconds{1};
                continue;
            }
        }

        // 组装本次要落库的日程实例，并应用未物化例外中的覆盖字段。
        Schedule schedule = ScheduleFactory::CreateOccurrence(*rule.value, *next);
        if (maybe_exception.has_value()) ScheduleFactory::ApplyOverride(schedule, *maybe_exception);
        schedule.rule_id = command.rule_id;

        // 由仓储事务完成 schedule 插入和 exception.schedule_id 回写。
        const Result<Schedule> inserted = rule_repository_.CreateNextInstance(schedule, maybe_exception);
        if (!inserted.ok()) {
            return FailedGenerateNextScheduleInstanceResult(inserted.status);
        }

        return {.status = Status::Ok(), .schedule = inserted.value, .error = {}};
    }
    return FailedGenerateNextScheduleInstanceResult(Status::Error(ErrorCode::kInternal, "生成下一条实例超出迭代上限"));
}

}  // namespace voicelife::schedule
