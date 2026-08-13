#include "voicelife/schedule/schedule_rule_service.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <utility>

#include "../rules/recurrence_planner.h"
#include "../rules/schedule_time_rules.h"

namespace voicelife::schedule {
namespace {

constexpr std::size_t kMaximumEventLength = 100;

DateTime Now() { return std::chrono::time_point_cast<std::chrono::seconds>(std::chrono::system_clock::now()); }

int64_t LocalTimeToSeconds(const LocalTime& value) { return value.hour * 3600 + value.minute * 60 + value.second; }

int CompareLocalDate(const LocalDate& left, const LocalDate& right) {
    if (left.year != right.year) return left.year < right.year ? -1 : 1;
    if (left.month != right.month) return left.month < right.month ? -1 : 1;
    if (left.day != right.day) return left.day < right.day ? -1 : 1;
    return 0;
}

/// 校验周期规则参数。
Status ValidateRule(const ScheduleRule& rule) {
    if (rule.event.empty()) return Status::Error(ErrorCode::kInvalidArgument, "规则名称不能为空");
    if (rule.event.length() > kMaximumEventLength) return Status::Error(ErrorCode::kInvalidArgument, "规则名称不能超过 100 个字符");
    if (rule.interval_val < 1) return Status::Error(ErrorCode::kInvalidArgument, "周期间隔必须大于零");
    switch (rule.freq_type) {
        case Frequency::kWeekly:
            if (!rule.weekdays_mask.has_value() || *rule.weekdays_mask < 1 || *rule.weekdays_mask > 127) {
                return Status::Error(ErrorCode::kInvalidArgument, "每周规则必须提供有效的星期位图");
            }
            break;
        case Frequency::kMonthly:
            if (!rule.monthly_mode.has_value()) return Status::Error(ErrorCode::kInvalidArgument, "每月规则必须提供月模式");
            if (*rule.monthly_mode == MonthlyMode::kSpecificDay && !rule.day_of_month.has_value()) {
                return Status::Error(ErrorCode::kInvalidArgument, "指定日期模式必须提供日期");
            }
            break;
        case Frequency::kYearly:
            if (!rule.month_of_year.has_value() || !rule.day_of_month.has_value()) {
                return Status::Error(ErrorCode::kInvalidArgument, "每年规则必须提供月份和日期");
            }
            break;
        case Frequency::kDaily:
            break;
    }
    if (rule.end_time.has_value() && LocalTimeToSeconds(*rule.end_time) <= LocalTimeToSeconds(rule.start_time)) {
        return Status::Error(ErrorCode::kInvalidArgument, "规则结束时间必须晚于开始时间");
    }
    if (rule.end_date.has_value() && CompareLocalDate(*rule.end_date, rule.start_date) < 0) {
        return Status::Error(ErrorCode::kInvalidArgument, "规则失效日期不能早于生效日期");
    }
    if (rule.end_date.has_value() && rule.occurrence_count.has_value()) {
        return Status::Error(ErrorCode::kInvalidArgument, "失效日期与最大次数只能二选一");
    }
    return Status::Ok();
}

/// 用规则默认值构造一条实例。
Schedule MakeSchedule(const ScheduleRule& rule, DateTime occurrence) {
    Schedule schedule;
    schedule.id = 0;
    schedule.event = rule.event;
    schedule.start_time = occurrence;
    if (rule.end_time.has_value()) {
        const int64_t duration = LocalTimeToSeconds(*rule.end_time) - LocalTimeToSeconds(rule.start_time);
        schedule.end_time = occurrence + std::chrono::seconds{duration};
    }
    schedule.location = rule.location;
    schedule.notes = rule.notes;
    schedule.rule_id = std::nullopt;
    schedule.status = ScheduleStatus::kActive;
    return schedule;
}

/// 将例外覆盖字段应用到实例。
void ApplyOverride(Schedule& schedule, const ScheduleException& exception) {
    if (exception.override_start_time.has_value()) schedule.start_time = exception.override_start_time;
    if (exception.override_end_time.has_value()) schedule.end_time = exception.override_end_time;
    if (exception.override_event.has_value()) schedule.event = *exception.override_event;
    if (exception.override_location.has_value()) schedule.location = exception.override_location;
    if (exception.override_notes.has_value()) schedule.notes = exception.override_notes;
}

/// 在实例集合中按 (rule_id, start_time) 查找已物化实例。
std::optional<Schedule> FindScheduleByRuleAndTime(ScheduleRuleId rule_id, DateTime time,
                                                  const std::vector<Schedule>& schedules) {
    for (const Schedule& schedule : schedules) {
        if (schedule.rule_id.has_value() && *schedule.rule_id == rule_id && schedule.start_time.has_value() &&
            *schedule.start_time == time) {
            return schedule;
        }
    }
    return std::nullopt;
}

/// 计算规则在 from 之后的前 n 次发生时间。
std::vector<DateTime> NextOccurrences(const ScheduleRule& rule, DateTime from, int n) {
    std::vector<DateTime> result;
    DateTime cursor = from;
    for (int index = 0; index < n; ++index) {
        const std::optional<DateTime> next = NextOccurrence(rule, cursor);
        if (!next.has_value()) break;
        result.push_back(*next);
        cursor = *next + std::chrono::seconds{1};
    }
    return result;
}

/// 判断关键词是否命中规则。
bool MatchesKeyword(const ScheduleRule& rule, const std::string& keyword) {
    if (keyword.empty()) return true;
    if (rule.event.find(keyword) != std::string::npos) return true;
    if (rule.location.has_value() && rule.location->find(keyword) != std::string::npos) return true;
    if (rule.notes.has_value() && rule.notes->find(keyword) != std::string::npos) return true;
    return false;
}

/// 判断规则状态是否命中筛选。
bool MatchesStatus(const ScheduleRule& rule, ScheduleStatusFilter filter) {
    switch (filter) {
        case ScheduleStatusFilter::kAll:
            return true;
        case ScheduleStatusFilter::kActive:
            return rule.status == ScheduleStatus::kActive;
        case ScheduleStatusFilter::kCancelled:
            return rule.status == ScheduleStatus::kCancelled;
        case ScheduleStatusFilter::kCompleted:
            return rule.status == ScheduleStatus::kCompleted;
    }
    return false;
}

}  // namespace

ScheduleRuleService::ScheduleRuleService(ScheduleRuleRepository& rule_repository,
                                         ScheduleExceptionRepository& exception_repository,
                                         ScheduleRepository& schedule_repository)
    : rule_repository_(rule_repository), exception_repository_(exception_repository),
      schedule_repository_(schedule_repository) {}

CreateScheduleRuleResult ScheduleRuleService::create_schedule_rule(const CreateScheduleRuleCommand& command) const {
    ScheduleRule rule{
        .id = 0,
        .event = command.event,
        .location = command.location,
        .notes = command.notes,
        .freq_type = command.freq_type,
        .interval_val = command.interval_val,
        .weekdays_mask = command.weekdays_mask,
        .day_of_month = command.day_of_month,
        .month_of_year = command.month_of_year,
        .monthly_mode = command.monthly_mode,
        .start_time = command.start_time,
        .end_time = command.end_time,
        .start_date = command.start_date,
        .end_date = command.end_date,
        .occurrence_count = command.occurrence_count,
        .status = ScheduleStatus::kActive,
        .created_at = {},
        .updated_at = {},
    };
    const Status validation = ValidateRule(rule);
    if (!validation.ok()) {
        return {.status = validation, .rule = std::nullopt, .schedules = {}, .conflicts = {}, .error = validation.message};
    }

    const DateTime now = Now();
    const std::optional<DateTime> first_time = NextOccurrence(rule, now);
    std::optional<Schedule> first_instance;
    if (first_time.has_value()) first_instance = MakeSchedule(rule, *first_time);

    // 冲突检测：首条实例与已有 active 日程重叠。
    std::vector<Schedule> conflicts;
    if (first_instance.has_value() && first_instance->start_time.has_value()) {
        const Result<std::vector<Schedule>> loaded = schedule_repository_.FindAll();
        if (!loaded.ok()) {
            return {.status = loaded.status, .rule = std::nullopt, .schedules = {}, .conflicts = {},
                    .error = "读取现有日程失败：" + loaded.status.message};
        }
        for (const Schedule& existing : *loaded.value) {
            if (existing.status != ScheduleStatus::kActive || !existing.start_time.has_value()) continue;
            if (SchedulesConflict(*first_instance, existing)) conflicts.push_back(existing);
        }
        if (!conflicts.empty() && !command.ignore_conflict) {
            return {.status = Status::Error(ErrorCode::kConflict, "首条实例与已有日程冲突"),
                    .rule = std::nullopt,
                    .schedules = {},
                    .conflicts = std::move(conflicts),
                    .error = "首条实例与已有日程冲突"};
        }
    }

    const Result<ScheduleRule> created = rule_repository_.CreateWithFirstInstance(rule, first_instance);
    if (!created.ok()) {
        return {.status = created.status, .rule = std::nullopt, .schedules = {}, .conflicts = std::move(conflicts),
                .error = created.status.message};
    }

    std::vector<Schedule> schedules;
    if (first_instance.has_value()) {
        first_instance->rule_id = created.value->id;
        schedules.push_back(*first_instance);
    }
    return {.status = Status::Ok(), .rule = created.value, .schedules = std::move(schedules),
            .conflicts = std::move(conflicts), .error = {}};
}

QueryScheduleRulesResult ScheduleRuleService::query_schedule_rules(const QueryScheduleRulesCommand& command) const {
    const Result<std::vector<ScheduleRule>> loaded = rule_repository_.FindAll();
    if (!loaded.ok()) {
        return {.status = loaded.status, .rules = {}, .total = 0, .error = loaded.status.message};
    }

    const DateTime now = Now();
    std::vector<ScheduleRuleView> views;
    for (const ScheduleRule& rule : *loaded.value) {
        if (command.rule_id.has_value() && rule.id != *command.rule_id) continue;
        if (command.keyword.has_value() && !MatchesKeyword(rule, *command.keyword)) continue;
        if (!MatchesStatus(rule, command.status)) continue;

        ScheduleRuleView view;
        view.rule = rule;
        view.upcoming_occurrences = NextOccurrences(rule, now, 3);
        const Result<std::vector<ScheduleException>> exceptions = exception_repository_.FindByRule(rule.id);
        if (!exceptions.ok()) {
            return {.status = exceptions.status, .rules = {}, .total = 0, .error = exceptions.status.message};
        }
        view.exceptions = *exceptions.value;
        views.push_back(std::move(view));
    }

    const int64_t total = static_cast<int64_t>(views.size());
    const std::size_t begin = command.offset >= total ? views.size() : static_cast<std::size_t>(command.offset);
    const std::size_t count = std::min(static_cast<std::size_t>(command.limit), views.size() - begin);
    std::vector<ScheduleRuleView> page(views.begin() + static_cast<std::ptrdiff_t>(begin),
                                       views.begin() + static_cast<std::ptrdiff_t>(begin + count));
    return {.status = Status::Ok(), .rules = std::move(page), .total = total, .error = {}};
}

UpdateScheduleRuleResult ScheduleRuleService::update_schedule_rule(const UpdateScheduleRuleCommand& command) {
    if (command.rule_id <= 0) {
        return {.status = Status::Error(ErrorCode::kInvalidArgument, "规则 ID 必须大于零"),
                .rule = std::nullopt, .schedules = {}, .conflicts = {}, .error = "规则 ID 必须大于零"};
    }
    const Result<ScheduleRule> loaded = rule_repository_.FindById(command.rule_id);
    if (!loaded.ok()) {
        return {.status = loaded.status, .rule = std::nullopt, .schedules = {}, .conflicts = {},
                .error = loaded.status.message};
    }

    ScheduleRule rule = *loaded.value;
    if (command.event.has_value()) rule.event = *command.event;
    if (command.location.has_value()) rule.location = *command.location;
    if (command.notes.has_value()) rule.notes = *command.notes;
    if (command.freq_type.has_value()) rule.freq_type = *command.freq_type;
    if (command.interval_val.has_value()) rule.interval_val = *command.interval_val;
    if (command.weekdays_mask.has_value()) rule.weekdays_mask = *command.weekdays_mask;
    if (command.day_of_month.has_value()) rule.day_of_month = *command.day_of_month;
    if (command.month_of_year.has_value()) rule.month_of_year = *command.month_of_year;
    if (command.monthly_mode.has_value()) rule.monthly_mode = *command.monthly_mode;
    if (command.start_time.has_value()) rule.start_time = *command.start_time;
    if (command.end_time.has_value()) rule.end_time = *command.end_time;
    if (command.start_date.has_value()) rule.start_date = *command.start_date;
    if (command.end_date.has_value()) rule.end_date = *command.end_date;
    if (command.occurrence_count.has_value()) rule.occurrence_count = *command.occurrence_count;

    const Status validation = ValidateRule(rule);
    if (!validation.ok()) {
        return {.status = validation, .rule = std::nullopt, .schedules = {}, .conflicts = {}, .error = validation.message};
    }

    const DateTime now = Now();
    const std::optional<DateTime> first_time = NextOccurrence(rule, now);
    std::optional<Schedule> first_instance;
    if (first_time.has_value()) first_instance = MakeSchedule(rule, *first_time);

    const Result<ScheduleRule> updated = rule_repository_.UpdateAndRebuild(rule, first_instance);
    if (!updated.ok()) {
        return {.status = updated.status, .rule = std::nullopt, .schedules = {}, .conflicts = {},
                .error = updated.status.message};
    }

    std::vector<Schedule> schedules;
    if (first_instance.has_value()) {
        first_instance->rule_id = rule.id;
        schedules.push_back(*first_instance);
    }
    return {.status = Status::Ok(), .rule = updated.value, .schedules = std::move(schedules), .conflicts = {}, .error = {}};
}

CancelScheduleRuleResult ScheduleRuleService::cancel_schedule_rule(const CancelScheduleRuleCommand& command) {
    if (command.rule_id <= 0) {
        return {.status = Status::Error(ErrorCode::kInvalidArgument, "规则 ID 必须大于零"),
                .rule = std::nullopt, .cancelled_count = 0, .error = "规则 ID 必须大于零"};
    }
    int64_t cancelled_count = 0;
    const Status cancelled = rule_repository_.CancelAndCancelFuture(command.rule_id, cancelled_count);
    if (!cancelled.ok()) {
        return {.status = cancelled, .rule = std::nullopt, .cancelled_count = 0, .error = cancelled.message};
    }
    const Result<ScheduleRule> rule = rule_repository_.FindById(command.rule_id);
    return {.status = Status::Ok(), .rule = rule.ok() ? rule.value : std::nullopt, .cancelled_count = cancelled_count,
            .error = {}};
}

UpdateScheduleOccurrenceResult ScheduleRuleService::update_schedule_occurrence(
    const UpdateScheduleOccurrenceCommand& command) {
    if (command.rule_id <= 0) {
        return {.status = Status::Error(ErrorCode::kInvalidArgument, "规则 ID 必须大于零"),
                .schedule = std::nullopt, .exception = std::nullopt, .conflicts = {}, .error = "规则 ID 必须大于零"};
    }
    const Result<ScheduleRule> rule = rule_repository_.FindById(command.rule_id);
    if (!rule.ok()) {
        return {.status = rule.status, .schedule = std::nullopt, .exception = std::nullopt, .conflicts = {},
                .error = rule.status.message};
    }

    // 读取或构造例外。
    ScheduleException exception;
    const Result<std::optional<ScheduleException>> existing =
        exception_repository_.FindByRuleAndTime(command.rule_id, command.original_start_time);
    if (!existing.ok()) {
        return {.status = existing.status, .schedule = std::nullopt, .exception = std::nullopt, .conflicts = {},
                .error = existing.status.message};
    }
    exception = (*existing.value).value_or(ScheduleException{});
    if (exception.rule_id == 0) {
        exception.rule_id = command.rule_id;
        exception.original_start_time = command.original_start_time;
        exception.type = ExceptionType::kModify;
    }
    if (command.event.has_value()) exception.override_event = *command.event;
    if (command.start_time.has_value()) exception.override_start_time = *command.start_time;
    if (command.end_time.has_value()) exception.override_end_time = *command.end_time;
    if (command.location.has_value()) exception.override_location = *command.location;
    if (command.notes.has_value()) exception.override_notes = *command.notes;

    // 查找已物化实例。
    const Result<std::vector<Schedule>> loaded = schedule_repository_.FindAll();
    if (!loaded.ok()) {
        return {.status = loaded.status, .schedule = std::nullopt, .exception = std::nullopt, .conflicts = {},
                .error = loaded.status.message};
    }
    std::optional<Schedule> materialized = FindScheduleByRuleAndTime(command.rule_id, command.original_start_time, *loaded.value);

    if (materialized.has_value()) {
        // 更新已物化实例并写入例外。
        Schedule updated = *materialized;
        ApplyOverride(updated, exception);
        const Status saved = schedule_repository_.Update(updated);
        if (!saved.ok()) {
            return {.status = saved, .schedule = std::nullopt, .exception = std::nullopt, .conflicts = {},
                    .error = saved.message};
        }
        exception.schedule_id = materialized->id;
        const Result<ScheduleException> upserted = exception_repository_.Upsert(exception);
        if (!upserted.ok()) {
            return {.status = upserted.status, .schedule = std::nullopt, .exception = std::nullopt, .conflicts = {},
                    .error = upserted.status.message};
        }
        return {.status = Status::Ok(), .schedule = updated, .exception = upserted.value, .conflicts = {}, .error = {}};
    }

    // 未物化：只写例外。
    const Result<ScheduleException> upserted = exception_repository_.Upsert(exception);
    if (!upserted.ok()) {
        return {.status = upserted.status, .schedule = std::nullopt, .exception = std::nullopt, .conflicts = {},
                .error = upserted.status.message};
    }
    return {.status = Status::Ok(), .schedule = std::nullopt, .exception = upserted.value, .conflicts = {}, .error = {}};
}

SkipScheduleOccurrenceResult ScheduleRuleService::skip_schedule_occurrence(const SkipScheduleOccurrenceCommand& command) {
    if (command.rule_id <= 0) {
        return {.status = Status::Error(ErrorCode::kInvalidArgument, "规则 ID 必须大于零"),
                .schedule = std::nullopt, .exception = std::nullopt, .error = "规则 ID 必须大于零"};
    }

    ScheduleException exception;
    exception.rule_id = command.rule_id;
    exception.original_start_time = command.original_start_time;
    exception.type = ExceptionType::kSkip;

    const Result<std::vector<Schedule>> loaded = schedule_repository_.FindAll();
    if (!loaded.ok()) {
        return {.status = loaded.status, .schedule = std::nullopt, .exception = std::nullopt, .error = loaded.status.message};
    }
    std::optional<Schedule> materialized = FindScheduleByRuleAndTime(command.rule_id, command.original_start_time, *loaded.value);

    std::optional<Schedule> cancelled_schedule;
    if (materialized.has_value()) {
        const Status deleted = schedule_repository_.Delete(materialized->id);
        if (!deleted.ok()) {
            return {.status = deleted, .schedule = std::nullopt, .exception = std::nullopt, .error = deleted.message};
        }
        exception.schedule_id = materialized->id;
        cancelled_schedule = *materialized;
        cancelled_schedule->status = ScheduleStatus::kCancelled;
    }

    const Result<ScheduleException> upserted = exception_repository_.Upsert(exception);
    if (!upserted.ok()) {
        return {.status = upserted.status, .schedule = std::nullopt, .exception = std::nullopt, .error = upserted.status.message};
    }
    return {.status = Status::Ok(), .schedule = cancelled_schedule, .exception = upserted.value, .error = {}};
}

GenerateNextScheduleInstanceResult ScheduleRuleService::generate_next_schedule_instance(
    const GenerateNextScheduleInstanceCommand& command) {
    if (command.rule_id <= 0) {
        return {.status = Status::Error(ErrorCode::kInvalidArgument, "规则 ID 必须大于零"),
                .schedule = std::nullopt, .error = "规则 ID 必须大于零"};
    }
    const Result<ScheduleRule> rule = rule_repository_.FindById(command.rule_id);
    if (!rule.ok()) {
        return {.status = rule.status, .schedule = std::nullopt, .error = rule.status.message};
    }

    const Result<std::vector<Schedule>> loaded = schedule_repository_.FindAll();
    if (!loaded.ok()) {
        return {.status = loaded.status, .schedule = std::nullopt, .error = loaded.status.message};
    }

    DateTime cursor = Now();
    for (int attempt = 0; attempt < 1000; ++attempt) {
        const std::optional<DateTime> next = NextOccurrence(*rule.value, cursor);
        if (!next.has_value()) {
            return {.status = Status::Ok(), .schedule = std::nullopt, .error = {}};
        }
        // 已物化则继续找下一条。
        if (FindScheduleByRuleAndTime(command.rule_id, *next, *loaded.value).has_value()) {
            cursor = *next + std::chrono::seconds{1};
            continue;
        }
        // 检查例外。
        const Result<std::optional<ScheduleException>> existing =
            exception_repository_.FindByRuleAndTime(command.rule_id, *next);
        if (!existing.ok()) {
            return {.status = existing.status, .schedule = std::nullopt, .error = existing.status.message};
        }
        const std::optional<ScheduleException>& maybe_exception = *existing.value;
        if (maybe_exception.has_value() && maybe_exception->type == ExceptionType::kSkip) {
            cursor = *next + std::chrono::seconds{1};
            continue;
        }
        Schedule schedule = MakeSchedule(*rule.value, *next);
        if (maybe_exception.has_value()) ApplyOverride(schedule, *maybe_exception);
        schedule.rule_id = command.rule_id;
        const Result<Schedule> inserted = schedule_repository_.Insert(schedule);
        if (!inserted.ok()) {
            return {.status = inserted.status, .schedule = std::nullopt, .error = inserted.status.message};
        }
        return {.status = Status::Ok(), .schedule = inserted.value, .error = {}};
    }
    return {.status = Status::Error(ErrorCode::kInternal, "生成下一条实例超出迭代上限"),
            .schedule = std::nullopt, .error = "生成下一条实例超出迭代上限"};
}

}  // namespace voicelife::schedule
