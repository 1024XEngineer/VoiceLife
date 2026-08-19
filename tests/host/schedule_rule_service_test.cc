#include "voicelife/schedule/schedule_rule_service.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "support/in_memory_schedule_repository.h"
#include "support/test_support.h"
#include "voicelife/schedule/calendar.h"
#include "voicelife/schedule/schedule_exception_repository.h"
#include "voicelife/schedule/schedule_rule_repository.h"

using voicelife::ErrorCode;
using voicelife::schedule::DateTime;
using voicelife::schedule::ExceptionType;
using voicelife::schedule::Frequency;
using voicelife::schedule::LocalDate;
using voicelife::schedule::LocalTime;
using voicelife::schedule::MonthlyMode;
using voicelife::schedule::Schedule;
using voicelife::schedule::ScheduleException;
using voicelife::schedule::ScheduleRule;
using voicelife::schedule::ScheduleRuleService;
using voicelife::schedule::ScheduleStatus;
using voicelife::schedule::ScheduleStatusFilter;
using voicelife::test::Check;
using voicelife::test::InMemoryScheduleRepository;

namespace {

/** @brief 按东八区本地时间构造 Unix 秒。 @param year 年。 @param month 月。 @param day 日。 @param hour 时。 @return
 * Unix 秒。 */
int64_t UtcAtLocal(int year, int month, int day, int hour) {
    return voicelife::schedule::DaysFromCivil(year, month, day) * 86400 + hour * 3600 - 8 * 3600;
}

/** @brief 转换 Unix 秒。 @param seconds Unix 秒。 @return 日程时间。 */
DateTime At(int64_t seconds) { return DateTime{std::chrono::seconds{seconds}}; }

/** @brief 构造默认的每日周期规则创建命令。 @param event 规则名称。 @return 创建命令。 */
voicelife::schedule::CreateScheduleRuleCommand DailyCommand(const std::string& event) {
    return {
        .event = event,
        .freq_type = Frequency::kDaily,
        .start_time = LocalTime{9, 0, 0},
        .start_date = LocalDate{2099, 3, 1},
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
        .ignore_conflict = false,
    };
}

/** @brief 测试用的内存单次例外仓储。 */
class FakeExceptionRepository final : public voicelife::schedule::ScheduleExceptionRepository {
   public:
    /**
     * @brief 插入或更新单次例外。
     * @param exception 待写入例外。
     * @return 保存后的例外。
     */
    voicelife::Result<ScheduleException> Upsert(const ScheduleException& exception) override {
        if (fail_upsert_.has_value()) {
            voicelife::Status failure = std::move(*fail_upsert_);
            fail_upsert_.reset();
            return voicelife::Result<ScheduleException>::Failure(failure.code, failure.message);
        }
        ScheduleException stored = exception;
        for (ScheduleException& existing : exceptions) {
            if (existing.rule_id == exception.rule_id &&
                existing.original_start_time == exception.original_start_time) {
                stored.id = existing.id;
                existing = stored;
                return voicelife::Result<ScheduleException>::Success(std::move(existing));
            }
        }
        stored.id = next_id_++;
        exceptions.push_back(stored);
        return voicelife::Result<ScheduleException>::Success(std::move(stored));
    }

    /**
     * @brief 返回指定规则的全部例外。
     * @param rule_id 规则标识。
     * @return 例外集合。
     */
    [[nodiscard]] voicelife::Result<std::vector<ScheduleException>> FindByRule(
        voicelife::schedule::ScheduleRuleId rule_id) const override {
        if (fail_find_by_rule_.has_value()) {
            voicelife::Status failure = std::move(*fail_find_by_rule_);
            fail_find_by_rule_.reset();
            return voicelife::Result<std::vector<ScheduleException>>::Failure(failure.code, failure.message);
        }
        std::vector<ScheduleException> matched;
        for (const ScheduleException& exception : exceptions) {
            if (exception.rule_id == rule_id) matched.push_back(exception);
        }
        return voicelife::Result<std::vector<ScheduleException>>::Success(std::move(matched));
    }

    /**
     * @brief 按规则和原始发生时间查询例外。
     * @param rule_id 规则标识。
     * @param original_start_time 原始发生时间。
     * @return 可空例外。
     */
    [[nodiscard]] voicelife::Result<std::optional<ScheduleException>> FindByRuleAndTime(
        voicelife::schedule::ScheduleRuleId rule_id, DateTime original_start_time) const override {
        if (fail_find_by_rule_and_time_.has_value()) {
            voicelife::Status failure = std::move(*fail_find_by_rule_and_time_);
            fail_find_by_rule_and_time_.reset();
            return voicelife::Result<std::optional<ScheduleException>>::Failure(failure.code, failure.message);
        }
        for (const ScheduleException& exception : exceptions) {
            if (exception.rule_id == rule_id && exception.original_start_time == original_start_time) {
                return voicelife::Result<std::optional<ScheduleException>>::Success(exception);
            }
        }
        return voicelife::Result<std::optional<ScheduleException>>::Success(std::nullopt);
    }

    /**
     * @brief 删除未来例外。
     * @param rule_id 规则标识。
     * @param after 边界时间。
     * @return 成功状态。
     */
    voicelife::Status DeleteFuture(voicelife::schedule::ScheduleRuleId rule_id, DateTime after) override {
        std::erase_if(exceptions, [rule_id, after](const ScheduleException& exception) {
            return exception.rule_id == rule_id && exception.original_start_time > after;
        });
        return voicelife::Status::Ok();
    }

    std::vector<ScheduleException> exceptions;
    int64_t next_id_ = 900;
    mutable std::optional<voicelife::Status> fail_find_by_rule_;
    mutable std::optional<voicelife::Status> fail_find_by_rule_and_time_;
    std::optional<voicelife::Status> fail_upsert_;
};

/** @brief 测试用的内存周期规则仓储。 */
class FakeRuleRepository final : public voicelife::schedule::ScheduleRuleRepository {
   public:
    /**
     * @brief 使用日程仓储和例外仓储构造规则仓储。
     * @param schedules 日程仓储。
     * @param exceptions 例外仓储。
     */
    FakeRuleRepository(InMemoryScheduleRepository& schedules, FakeExceptionRepository& exceptions)
        : schedules_(schedules), exceptions_(exceptions) {}

    /**
     * @brief 插入规则。
     * @param rule 待插入规则。
     * @return 保存后的规则。
     */
    voicelife::Result<ScheduleRule> Insert(const ScheduleRule& rule) override {
        ScheduleRule stored = rule;
        stored.id = next_id_++;
        rules.push_back(stored);
        return voicelife::Result<ScheduleRule>::Success(std::move(stored));
    }

    /**
     * @brief 更新已有规则。
     * @param rule 待更新规则。
     * @return 成功或未找到。
     */
    voicelife::Status Update(const ScheduleRule& rule) override {
        for (ScheduleRule& existing : rules) {
            if (existing.id == rule.id) {
                existing = rule;
                return voicelife::Status::Ok();
            }
        }
        return voicelife::Status::Error(ErrorCode::kNotFound, "规则不存在");
    }

    /** @brief 返回全部规则。 @return 规则集合。 */
    [[nodiscard]] voicelife::Result<std::vector<ScheduleRule>> FindAll() const override {
        if (fail_find_all_.has_value()) {
            voicelife::Status failure = std::move(*fail_find_all_);
            fail_find_all_.reset();
            return voicelife::Result<std::vector<ScheduleRule>>::Failure(failure.code, failure.message);
        }
        return voicelife::Result<std::vector<ScheduleRule>>::Success(rules);
    }

    /**
     * @brief 按标识读取规则。
     * @param id 规则标识。
     * @return 规则或错误。
     */
    [[nodiscard]] voicelife::Result<ScheduleRule> FindById(voicelife::schedule::ScheduleRuleId id) const override {
        if (fail_find_by_id_once_.has_value()) {
            voicelife::Status failure = std::move(*fail_find_by_id_once_);
            fail_find_by_id_once_.reset();
            return voicelife::Result<ScheduleRule>::Failure(failure.code, failure.message);
        }
        for (const ScheduleRule& rule : rules) {
            if (rule.id == id) return voicelife::Result<ScheduleRule>::Success(rule);
        }
        return voicelife::Result<ScheduleRule>::Failure(ErrorCode::kNotFound, "规则不存在");
    }

    /**
     * @brief 创建规则和首条实例。
     * @param rule 待创建规则。
     * @param first_instance 可空首条实例。
     * @return 保存后的规则。
     */
    voicelife::Result<ScheduleRule> CreateWithFirstInstance(const ScheduleRule& rule,
                                                            const std::optional<Schedule>& first_instance) override {
        if (fail_create_.has_value()) {
            voicelife::Status failure = std::move(*fail_create_);
            fail_create_.reset();
            return voicelife::Result<ScheduleRule>::Failure(failure.code, failure.message);
        }
        const auto created = Insert(rule);
        if (!created.ok()) return created;
        if (first_instance.has_value()) {
            Schedule instance = *first_instance;
            instance.rule_id = created.value->id;
            (void)schedules_.Insert(instance);
        }
        return created;
    }

    /**
     * @brief 更新规则并重建首条实例。
     * @param rule 待更新规则。
     * @param first_instance 新首条实例。
     * @return 更新后的规则。
     */
    voicelife::Result<ScheduleRule> UpdateAndRebuild(const ScheduleRule& rule,
                                                     const std::optional<Schedule>& first_instance) override {
        if (fail_update_rebuild_.has_value()) {
            voicelife::Status failure = std::move(*fail_update_rebuild_);
            fail_update_rebuild_.reset();
            return voicelife::Result<ScheduleRule>::Failure(failure.code, failure.message);
        }
        const voicelife::Status updated = Update(rule);
        if (!updated.ok()) return voicelife::Result<ScheduleRule>::Failure(updated.code, updated.message);
        if (first_instance.has_value()) {
            Schedule instance = *first_instance;
            instance.rule_id = rule.id;
            (void)schedules_.Insert(instance);
        }
        return FindById(rule.id);
    }

    /**
     * @brief 取消规则及其实例。
     * @param id 规则标识。
     * @param cancelled_instance_count 输出取消实例数。
     * @return 成功状态。
     */
    voicelife::Status CancelRuleAndInstances(voicelife::schedule::ScheduleRuleId id,
                                             int64_t& cancelled_instance_count) override {
        // 直接遍历规则集合，避免复用 FindById 触发失败注入影响取消流程的原子性。
        ScheduleRule* target = nullptr;
        for (ScheduleRule& rule : rules) {
            if (rule.id == id) {
                target = &rule;
                break;
            }
        }
        if (target == nullptr) return voicelife::Status::Error(ErrorCode::kNotFound, "规则不存在");
        ScheduleRule cancelled = *target;
        cancelled.status = ScheduleStatus::kCancelled;
        const voicelife::Status updated = Update(cancelled);
        if (!updated.ok()) return updated;
        cancelled_instance_count = 0;
        voicelife::schedule::QueryScheduleCommand query;
        query.rule_id = id;
        query.status = ScheduleStatusFilter::kAll;
        query.limit = 100;
        const auto loaded = schedules_.Find(query);
        if (!loaded.ok()) return loaded.status;
        for (Schedule schedule : *loaded.value) {
            if (schedule.status == ScheduleStatus::kActive) {
                schedule.status = ScheduleStatus::kCancelled;
                const voicelife::Status saved = schedules_.Update(schedule);
                if (!saved.ok()) return saved;
                ++cancelled_instance_count;
            }
        }
        return voicelife::Status::Ok();
    }

    /**
     * @brief 创建下一条实例并回写例外关联。
     * @param schedule 待插入实例。
     * @param linked_exception 可空关联例外。
     * @return 保存后的实例。
     */
    voicelife::Result<Schedule> CreateNextInstance(const Schedule& schedule,
                                                   const std::optional<ScheduleException>& linked_exception) override {
        if (fail_create_next_instance_.has_value()) {
            voicelife::Status failure = std::move(*fail_create_next_instance_);
            fail_create_next_instance_.reset();
            return voicelife::Result<Schedule>::Failure(failure.code, failure.message);
        }
        const auto inserted = schedules_.Insert(schedule);
        if (!inserted.ok()) return inserted;
        if (linked_exception.has_value()) {
            ScheduleException linked = *linked_exception;
            linked.schedule_id = inserted.value->id;
            (void)exceptions_.Upsert(linked);
        }
        return inserted;
    }

    std::vector<ScheduleRule> rules;
    int64_t next_id_ = 500;
    mutable std::optional<voicelife::Status> fail_find_all_;
    mutable std::optional<voicelife::Status> fail_find_by_id_once_;
    std::optional<voicelife::Status> fail_create_;
    std::optional<voicelife::Status> fail_update_rebuild_;
    std::optional<voicelife::Status> fail_create_next_instance_;

   private:
    InMemoryScheduleRepository& schedules_;
    FakeExceptionRepository& exceptions_;
};

}  // namespace

int main() {
    InMemoryScheduleRepository schedules;
    FakeExceptionRepository exceptions;
    FakeRuleRepository rules(schedules, exceptions);
    ScheduleRuleService service(rules, exceptions, schedules);

    const auto created = service.create_schedule_rule({
        .event = "每日例会",
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
    });
    Check(created.status.ok() && created.rule.has_value() && created.rule->id > 0 && created.schedules.size() == 1 &&
              created.schedules.front().start_time.has_value() &&
              created.schedules.front().start_time->time_since_epoch().count() == UtcAtLocal(2099, 1, 1, 9) &&
              created.schedules.front().rule_id == created.rule->id,
          "创建周期规则必须物化首条实例并回写规则 ID");

    ScheduleException modify;
    modify.rule_id = created.rule->id;
    modify.original_start_time = At(UtcAtLocal(2099, 1, 2, 9));
    modify.type = ExceptionType::kModify;
    modify.override_event = "修改后的第二场";
    (void)exceptions.Upsert(modify);
    const auto queried = service.query_schedule_rules({
        .rule_id = created.rule->id,
        .keyword = std::nullopt,
        .status = ScheduleStatusFilter::kAll,
        .occurrence_start = std::nullopt,
        .occurrence_end = std::nullopt,
        .limit = 10,
        .offset = 0,
    });
    Check(queried.status.ok() && queried.total == 1 && queried.rules.size() == 1 &&
              queried.rules.front().upcoming_occurrences.size() == 3 && queried.rules.front().exceptions.size() == 1,
          "查询周期规则必须返回未来发生时间和例外");

    // 明确的远期单日窗口必须直接规划目标 occurrence，不能被默认的未来三条预览截断。
    const auto ranged_query = service.query_schedule_rules({
        .rule_id = created.rule->id,
        .keyword = std::nullopt,
        .status = ScheduleStatusFilter::kAll,
        .occurrence_start = At(UtcAtLocal(2099, 1, 31, 0)),
        .occurrence_end = At(UtcAtLocal(2099, 2, 1, 0)),
        .limit = 10,
        .offset = 0,
    });
    Check(ranged_query.status.ok() && ranged_query.rules.size() == 1 &&
              ranged_query.rules.front().upcoming_occurrences.size() == 1 &&
              ranged_query.rules.front().upcoming_occurrences.front() == At(UtcAtLocal(2099, 1, 31, 9)),
          "周期规则按远期单日窗口查询时必须返回目标 occurrence");

    // 显式历史日期窗口也必须按窗口规划 occurrence，不能被“从现在开始”的默认预览逻辑吞掉。
    const auto historical = service.create_schedule_rule({
        .event = "历史周期回归",
        .freq_type = Frequency::kDaily,
        .start_time = LocalTime{9, 0, 0},
        .start_date = LocalDate{2020, 1, 1},
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
    });
    Check(historical.status.ok() && historical.rule.has_value(), "历史周期规则应能建立查询回归数据");
    const auto historical_query = service.query_schedule_rules({
        .rule_id = historical.rule->id,
        .keyword = std::nullopt,
        .status = ScheduleStatusFilter::kAll,
        .occurrence_start = At(UtcAtLocal(2020, 1, 2, 0)),
        .occurrence_end = At(UtcAtLocal(2020, 1, 3, 0)),
        .limit = 10,
        .offset = 0,
    });
    Check(historical_query.status.ok() && historical_query.rules.size() == 1 &&
              historical_query.rules.front().upcoming_occurrences.size() == 1 &&
              historical_query.rules.front().upcoming_occurrences.front() == At(UtcAtLocal(2020, 1, 2, 9)),
          "周期规则按历史单日窗口查询时必须返回目标 occurrence");

    const auto updated = service.update_schedule_rule({
        .rule_id = created.rule->id,
        .event = std::optional<std::string>{"新每日例会"},
        .location = std::nullopt,
        .notes = std::nullopt,
        .freq_type = std::nullopt,
        .interval_val = std::nullopt,
        .weekdays_mask = std::nullopt,
        .day_of_month = std::nullopt,
        .month_of_year = std::nullopt,
        .monthly_mode = std::nullopt,
        .start_time = std::nullopt,
        .start_date = std::nullopt,
        .end_time = std::nullopt,
        .end_date = std::nullopt,
        .occurrence_count = std::nullopt,
    });
    Check(updated.status.ok() && updated.rule.has_value() && updated.rule->event == "新每日例会" &&
              updated.schedules.size() == 1 && updated.schedules.front().event == "新每日例会",
          "更新周期规则必须保留未提供字段并重建下一条实例");

    const auto generated = service.generate_next_schedule_instance({.rule_id = created.rule->id});
    Check(generated.status.ok() && generated.schedule.has_value() &&
              generated.schedule->start_time == At(UtcAtLocal(2099, 1, 2, 9)),
          "生成下一条实例必须跳过已物化首条并创建下一发生时间");

    const auto skipped = service.skip_schedule_occurrence({
        .rule_id = created.rule->id,
        .original_start_time = At(UtcAtLocal(2099, 1, 4, 9)),
    });
    const auto skipped_again = service.skip_schedule_occurrence({
        .rule_id = created.rule->id,
        .original_start_time = At(UtcAtLocal(2099, 1, 4, 9)),
    });
    Check(skipped.status.ok() && skipped.exception.has_value() && skipped_again.status.ok() &&
              skipped_again.exception.has_value() && skipped.exception->id == skipped_again.exception->id,
          "跳过未来单次应幂等返回同一条例外");

    const auto materialized_conflict = service.update_schedule_occurrence({
        .rule_id = created.rule->id,
        .original_start_time = At(UtcAtLocal(2099, 1, 1, 9)),
        .event = std::optional<std::string>{"不能改"},
        .start_time = std::nullopt,
        .end_time = std::nullopt,
        .location = std::nullopt,
        .notes = std::nullopt,
        .ignore_conflict = false,
    });
    Check(materialized_conflict.status.code == ErrorCode::kConflict,
          "已物化实例必须走 update_schedule，不能通过 occurrence 修改");

    const auto cancelled = service.cancel_schedule_rule({.rule_id = created.rule->id});
    Check(cancelled.status.ok() && cancelled.rule.has_value() && cancelled.rule->status == ScheduleStatus::kCancelled &&
              cancelled.cancelled_count >= 2,
          "取消周期规则必须同时取消规则和已物化实例");

    // —— 失败路径与边界分支覆盖 ——

    // create：字段校验失败（周期间隔非法）。
    const auto create_bad_interval = service.create_schedule_rule({
        .event = "坏间隔",
        .freq_type = Frequency::kDaily,
        .start_time = LocalTime{9, 0, 0},
        .start_date = std::nullopt,
        .end_time = std::nullopt,
        .location = std::nullopt,
        .notes = std::nullopt,
        .interval_val = 0,
        .weekdays_mask = std::nullopt,
        .day_of_month = std::nullopt,
        .month_of_year = std::nullopt,
        .monthly_mode = std::nullopt,
        .end_date = std::nullopt,
        .occurrence_count = std::nullopt,
    });
    Check(create_bad_interval.status.code == ErrorCode::kInvalidArgument, "非法周期间隔的 create 应被拒绝");

    // create：失效日期早于开始日期，无法计算首个发生时间。
    const auto create_no_first = service.create_schedule_rule({
        .event = "无发生",
        .freq_type = Frequency::kDaily,
        .start_time = LocalTime{9, 0, 0},
        .start_date = LocalDate{2099, 1, 10},
        .end_time = std::nullopt,
        .location = std::nullopt,
        .notes = std::nullopt,
        .interval_val = 1,
        .weekdays_mask = std::nullopt,
        .day_of_month = std::nullopt,
        .month_of_year = std::nullopt,
        .monthly_mode = std::nullopt,
        .end_date = LocalDate{2099, 1, 5},
        .occurrence_count = std::nullopt,
    });
    Check(create_no_first.status.code == ErrorCode::kInvalidArgument, "无法计算首个发生时间的 create 应被拒绝");

    // update：非法规则标识。
    const auto update_bad_id = service.update_schedule_rule({
        .rule_id = 0,
        .event = std::nullopt,
        .location = std::nullopt,
        .notes = std::nullopt,
        .freq_type = std::nullopt,
        .interval_val = std::nullopt,
        .weekdays_mask = std::nullopt,
        .day_of_month = std::nullopt,
        .month_of_year = std::nullopt,
        .monthly_mode = std::nullopt,
        .start_time = std::nullopt,
        .start_date = std::nullopt,
        .end_time = std::nullopt,
        .end_date = std::nullopt,
        .occurrence_count = std::nullopt,
    });
    Check(update_bad_id.status.code == ErrorCode::kInvalidArgument, "非法规则标识的 update 应被拒绝");

    // update：字段校验失败（周期间隔非法）。
    const auto update_bad_interval = service.update_schedule_rule({
        .rule_id = created.rule->id,
        .event = std::nullopt,
        .location = std::nullopt,
        .notes = std::nullopt,
        .freq_type = std::nullopt,
        .interval_val = std::optional<int32_t>{0},
        .weekdays_mask = std::nullopt,
        .day_of_month = std::nullopt,
        .month_of_year = std::nullopt,
        .monthly_mode = std::nullopt,
        .start_time = std::nullopt,
        .start_date = std::nullopt,
        .end_time = std::nullopt,
        .end_date = std::nullopt,
        .occurrence_count = std::nullopt,
    });
    Check(update_bad_interval.status.code == ErrorCode::kInvalidArgument, "非法周期间隔的 update 应被拒绝");

    // cancel：非法规则标识与不存在规则。
    Check(service.cancel_schedule_rule({.rule_id = 0}).status.code == ErrorCode::kInvalidArgument,
          "非法规则标识的 cancel 应被拒绝");
    Check(service.cancel_schedule_rule({.rule_id = 999999}).status.code == ErrorCode::kNotFound,
          "取消不存在规则应返回未找到");

    // update_schedule_occurrence：非法规则标识。
    Check(service.update_schedule_occurrence({
                                                 .rule_id = 0,
                                                 .original_start_time = At(UtcAtLocal(2099, 1, 6, 9)),
                                                 .event = std::optional<std::string>{"x"},
                                                 .start_time = std::nullopt,
                                                 .end_time = std::nullopt,
                                                 .location = std::nullopt,
                                                 .notes = std::nullopt,
                                             })
                  .status.code == ErrorCode::kInvalidArgument,
          "非法规则标识的 occurrence.update 应被拒绝");

    // update_schedule_occurrence：不存在规则。
    Check(service.update_schedule_occurrence({
                                                 .rule_id = 999999,
                                                 .original_start_time = At(UtcAtLocal(2099, 1, 6, 9)),
                                                 .event = std::optional<std::string>{"x"},
                                                 .start_time = std::nullopt,
                                                 .end_time = std::nullopt,
                                                 .location = std::nullopt,
                                                 .notes = std::nullopt,
                                             })
                  .status.code == ErrorCode::kNotFound,
          "修改不存在规则的单次应返回未找到");

    // update_schedule_occurrence：未提供任何修改字段。
    Check(service.update_schedule_occurrence({
                                                 .rule_id = created.rule->id,
                                                 .original_start_time = At(UtcAtLocal(2099, 1, 6, 9)),
                                                 .event = std::nullopt,
                                                 .start_time = std::nullopt,
                                                 .end_time = std::nullopt,
                                                 .location = std::nullopt,
                                                 .notes = std::nullopt,
                                             })
                  .status.code == ErrorCode::kInvalidArgument,
          "未提供修改字段的 occurrence.update 应被拒绝");

    // skip：非法规则标识。
    Check(service.skip_schedule_occurrence({
                                               .rule_id = 0,
                                               .original_start_time = At(UtcAtLocal(2099, 1, 7, 9)),
                                           })
                  .status.code == ErrorCode::kInvalidArgument,
          "非法规则标识的 skip 应被拒绝");

    // generate_next：非法规则标识与不存在规则。
    Check(service.generate_next_schedule_instance({.rule_id = 0}).status.code == ErrorCode::kInvalidArgument,
          "非法规则标识的 generate_next 应被拒绝");
    Check(service.generate_next_schedule_instance({.rule_id = 999999}).status.code == ErrorCode::kNotFound,
          "生成不存在规则的下一条实例应返回未找到");

    // 新建规则用于已物化实例的跳过与过期规则边界分支。
    const auto fresh = service.create_schedule_rule({
        .event = "新规则",
        .freq_type = Frequency::kDaily,
        .start_time = LocalTime{9, 0, 0},
        .start_date = LocalDate{2099, 2, 1},
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
    });
    Check(fresh.status.ok(), "新建规则应成功");
    const auto skip_materialized = service.skip_schedule_occurrence({
        .rule_id = fresh.rule->id,
        .original_start_time = At(UtcAtLocal(2099, 2, 1, 9)),
    });
    Check(skip_materialized.status.code == ErrorCode::kConflict, "跳过已物化实例应返回冲突");

    // 预置已过期规则，生成下一条实例应返回空结果。
    ScheduleRule expired_rule;
    expired_rule.id = 770;
    expired_rule.event = "已过期";
    expired_rule.freq_type = Frequency::kDaily;
    expired_rule.interval_val = 1;
    expired_rule.start_time = LocalTime{9, 0, 0};
    expired_rule.start_date = LocalDate{2019, 1, 1};
    expired_rule.end_date = LocalDate{2020, 1, 1};
    expired_rule.status = ScheduleStatus::kActive;
    rules.rules.push_back(expired_rule);
    const auto generated_exhausted = service.generate_next_schedule_instance({.rule_id = 770});
    Check(generated_exhausted.status.ok() && !generated_exhausted.schedule.has_value(),
          "过期规则生成下一条实例应返回空");

    // update：失效日期早于开始日期，无法计算首个发生时间。
    const auto update_no_next = service.update_schedule_rule({
        .rule_id = fresh.rule->id,
        .event = std::nullopt,
        .location = std::nullopt,
        .notes = std::nullopt,
        .freq_type = std::nullopt,
        .interval_val = std::nullopt,
        .weekdays_mask = std::nullopt,
        .day_of_month = std::nullopt,
        .month_of_year = std::nullopt,
        .monthly_mode = std::nullopt,
        .start_time = std::nullopt,
        .start_date = std::optional<std::optional<LocalDate>>{std::optional<LocalDate>{LocalDate{2099, 2, 10}}},
        .end_time = std::nullopt,
        .end_date = std::optional<std::optional<LocalDate>>{std::optional<LocalDate>{LocalDate{2099, 2, 5}}},
        .occurrence_count = std::nullopt,
    });
    Check(update_no_next.status.code == ErrorCode::kInvalidArgument, "无法计算首个发生时间的 update 应被拒绝");

    // generate_next：命中跳过例外分支后继续推进。
    const auto skip_fresh = service.skip_schedule_occurrence({
        .rule_id = fresh.rule->id,
        .original_start_time = At(UtcAtLocal(2099, 2, 2, 9)),
    });
    Check(skip_fresh.status.ok(), "跳过 fresh 规则的次日应成功");
    const auto generated_after_skip = service.generate_next_schedule_instance({.rule_id = fresh.rule->id});
    Check(generated_after_skip.status.ok() && generated_after_skip.schedule.has_value() &&
              generated_after_skip.schedule->start_time == At(UtcAtLocal(2099, 2, 3, 9)),
          "generate_next 应跳过已跳过例外并生成下一发生时间");

    // —— 仓储失败路径：逐条注入失败，验证服务层把底层错误透传回去 ——
    {
        InMemoryScheduleRepository err_schedules;
        FakeExceptionRepository err_exceptions;
        FakeRuleRepository err_rules(err_schedules, err_exceptions);
        ScheduleRuleService err_service(err_rules, err_exceptions, err_schedules);

        // create：FindOverlapping 失败。
        err_schedules.FailNextFindOverlapping(voicelife::Status::Error(ErrorCode::kUnavailable, "读取现有日程失败"));
        Check(err_service.create_schedule_rule(DailyCommand("重叠查询失败")).status.code == ErrorCode::kUnavailable,
              "create 应透传 FindOverlapping 错误");

        // create：CreateWithFirstInstance 失败。
        err_rules.fail_create_ = voicelife::Status::Error(ErrorCode::kInternal, "事务写入失败");
        Check(err_service.create_schedule_rule(DailyCommand("落库失败")).status.code == ErrorCode::kInternal,
              "create 应透传 CreateWithFirstInstance 错误");

        // 创建一条基准规则，供后续 update / cancel / occurrence 用例复用有效规则标识。
        const auto err_created = err_service.create_schedule_rule(DailyCommand("基准规则"));
        Check(err_created.status.ok(), "基准规则应创建成功");

        // query：FindAll 失败。
        err_rules.fail_find_all_ = voicelife::Status::Error(ErrorCode::kUnavailable, "规则仓储不可用");
        Check(err_service
                      .query_schedule_rules({.rule_id = std::nullopt,
                                             .keyword = std::nullopt,
                                             .status = ScheduleStatusFilter::kAll,
                                             .occurrence_start = std::nullopt,
                                             .occurrence_end = std::nullopt,
                                             .limit = 10,
                                             .offset = 0})
                      .status.code == ErrorCode::kUnavailable,
              "query 应透传 FindAll 错误");

        // query：例外 FindByRule 失败。
        err_exceptions.fail_find_by_rule_ = voicelife::Status::Error(ErrorCode::kUnavailable, "例外仓储不可用");
        Check(err_service
                      .query_schedule_rules({.rule_id = err_created.rule->id,
                                             .keyword = std::nullopt,
                                             .status = ScheduleStatusFilter::kAll,
                                             .occurrence_start = std::nullopt,
                                             .occurrence_end = std::nullopt,
                                             .limit = 10,
                                             .offset = 0})
                      .status.code == ErrorCode::kUnavailable,
              "query 应透传例外 FindByRule 错误");

        // update：FindOverlapping 失败。
        err_schedules.FailNextFindOverlapping(voicelife::Status::Error(ErrorCode::kUnavailable, "读取现有日程失败"));
        Check(err_service
                      .update_schedule_rule({.rule_id = err_created.rule->id,
                                             .event = std::optional<std::string>{"改"},
                                             .location = std::nullopt,
                                             .notes = std::nullopt,
                                             .freq_type = std::nullopt,
                                             .interval_val = std::nullopt,
                                             .weekdays_mask = std::nullopt,
                                             .day_of_month = std::nullopt,
                                             .month_of_year = std::nullopt,
                                             .monthly_mode = std::nullopt,
                                             .start_time = std::nullopt,
                                             .start_date = std::nullopt,
                                             .end_time = std::nullopt,
                                             .end_date = std::nullopt,
                                             .occurrence_count = std::nullopt})
                      .status.code == ErrorCode::kUnavailable,
              "update 应透传 FindOverlapping 错误");

        // update：UpdateAndRebuild 失败。
        err_rules.fail_update_rebuild_ = voicelife::Status::Error(ErrorCode::kInternal, "重建事务失败");
        Check(err_service
                      .update_schedule_rule({.rule_id = err_created.rule->id,
                                             .event = std::optional<std::string>{"改"},
                                             .location = std::nullopt,
                                             .notes = std::nullopt,
                                             .freq_type = std::nullopt,
                                             .interval_val = std::nullopt,
                                             .weekdays_mask = std::nullopt,
                                             .day_of_month = std::nullopt,
                                             .month_of_year = std::nullopt,
                                             .monthly_mode = std::nullopt,
                                             .start_time = std::nullopt,
                                             .start_date = std::nullopt,
                                             .end_time = std::nullopt,
                                             .end_date = std::nullopt,
                                             .occurrence_count = std::nullopt})
                      .status.code == ErrorCode::kInternal,
              "update 应透传 UpdateAndRebuild 错误");

        // update_schedule_occurrence：例外 FindByRuleAndTime 失败。
        err_exceptions.fail_find_by_rule_and_time_ = voicelife::Status::Error(ErrorCode::kUnavailable, "例外查询失败");
        Check(err_service
                      .update_schedule_occurrence({.rule_id = err_created.rule->id,
                                                   .original_start_time = At(UtcAtLocal(2099, 3, 2, 9)),
                                                   .event = std::optional<std::string>{"改"},
                                                   .start_time = std::nullopt,
                                                   .end_time = std::nullopt,
                                                   .location = std::nullopt,
                                                   .notes = std::nullopt,
                                                   .ignore_conflict = false})
                      .status.code == ErrorCode::kUnavailable,
              "occurrence.update 应透传例外查询错误");

        // update_schedule_occurrence：物化实例查询失败。
        err_schedules.FailNextFind(voicelife::Status::Error(ErrorCode::kUnavailable, "日程查询失败"));
        Check(err_service
                      .update_schedule_occurrence({.rule_id = err_created.rule->id,
                                                   .original_start_time = At(UtcAtLocal(2099, 3, 2, 9)),
                                                   .event = std::optional<std::string>{"改"},
                                                   .start_time = std::nullopt,
                                                   .end_time = std::nullopt,
                                                   .location = std::nullopt,
                                                   .notes = std::nullopt,
                                                   .ignore_conflict = false})
                      .status.code == ErrorCode::kUnavailable,
              "occurrence.update 应透传物化实例查询错误");

        // update_schedule_occurrence：Upsert 失败。
        err_exceptions.fail_upsert_ = voicelife::Status::Error(ErrorCode::kInternal, "例外写入失败");
        Check(err_service
                      .update_schedule_occurrence({.rule_id = err_created.rule->id,
                                                   .original_start_time = At(UtcAtLocal(2099, 3, 2, 9)),
                                                   .event = std::optional<std::string>{"改"},
                                                   .start_time = std::nullopt,
                                                   .end_time = std::nullopt,
                                                   .location = std::nullopt,
                                                   .notes = std::nullopt,
                                                   .ignore_conflict = false})
                      .status.code == ErrorCode::kInternal,
              "occurrence.update 应透传 Upsert 错误");

        // skip：例外查询失败。
        err_exceptions.fail_find_by_rule_and_time_ = voicelife::Status::Error(ErrorCode::kUnavailable, "例外查询失败");
        Check(err_service
                      .skip_schedule_occurrence(
                          {.rule_id = err_created.rule->id, .original_start_time = At(UtcAtLocal(2099, 3, 4, 9))})
                      .status.code == ErrorCode::kUnavailable,
              "skip 应透传例外查询错误");

        // skip：物化实例查询失败。
        err_schedules.FailNextFind(voicelife::Status::Error(ErrorCode::kUnavailable, "日程查询失败"));
        Check(err_service
                      .skip_schedule_occurrence(
                          {.rule_id = err_created.rule->id, .original_start_time = At(UtcAtLocal(2099, 3, 4, 9))})
                      .status.code == ErrorCode::kUnavailable,
              "skip 应透传物化实例查询错误");

        // skip：Upsert 失败。
        err_exceptions.fail_upsert_ = voicelife::Status::Error(ErrorCode::kInternal, "例外写入失败");
        Check(err_service
                      .skip_schedule_occurrence(
                          {.rule_id = err_created.rule->id, .original_start_time = At(UtcAtLocal(2099, 3, 4, 9))})
                      .status.code == ErrorCode::kInternal,
              "skip 应透传 Upsert 错误");

        // generate_next：例外查询失败。
        err_exceptions.fail_find_by_rule_and_time_ = voicelife::Status::Error(ErrorCode::kUnavailable, "例外查询失败");
        Check(err_service.generate_next_schedule_instance({.rule_id = err_created.rule->id}).status.code ==
                  ErrorCode::kUnavailable,
              "generate_next 应透传例外查询错误");

        // generate_next：物化实例查询失败。
        err_schedules.FailNextFind(voicelife::Status::Error(ErrorCode::kUnavailable, "日程查询失败"));
        Check(err_service.generate_next_schedule_instance({.rule_id = err_created.rule->id}).status.code ==
                  ErrorCode::kUnavailable,
              "generate_next 应透传物化实例查询错误");

        // generate_next：CreateNextInstance 失败。
        err_rules.fail_create_next_instance_ = voicelife::Status::Error(ErrorCode::kInternal, "实例落库失败");
        Check(err_service.generate_next_schedule_instance({.rule_id = err_created.rule->id}).status.code ==
                  ErrorCode::kInternal,
              "generate_next 应透传 CreateNextInstance 错误");

        // generate_next：命中带 schedule_id 的例外时跳过该候选并继续推进到下一发生时间。
        ScheduleException linked_exception;
        linked_exception.rule_id = err_created.rule->id;
        linked_exception.original_start_time = At(UtcAtLocal(2099, 3, 2, 9));
        linked_exception.type = ExceptionType::kModify;
        linked_exception.schedule_id = 9001;
        err_exceptions.exceptions.push_back(linked_exception);
        const auto generated_past_linked =
            err_service.generate_next_schedule_instance({.rule_id = err_created.rule->id});
        Check(generated_past_linked.status.ok() && generated_past_linked.schedule.has_value() &&
                  generated_past_linked.schedule->start_time == At(UtcAtLocal(2099, 3, 3, 9)),
              "generate_next 应跳过带 schedule_id 的例外并生成下一发生时间");

        // cancel：取消成功后读取规则快照失败。
        err_rules.fail_find_by_id_once_ = voicelife::Status::Error(ErrorCode::kInternal, "读取取消后规则失败");
        Check(err_service.cancel_schedule_rule({.rule_id = err_created.rule->id}).status.code == ErrorCode::kInternal,
              "cancel 应透传取消后 FindById 错误");
    }
    return 0;
}
