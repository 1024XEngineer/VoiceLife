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

/** @brief 测试用的内存单次例外仓储。 */
class FakeExceptionRepository final : public voicelife::schedule::ScheduleExceptionRepository {
   public:
    /**
     * @brief 插入或更新单次例外。
     * @param exception 待写入例外。
     * @return 保存后的例外。
     */
    voicelife::Result<ScheduleException> Upsert(const ScheduleException& exception) override {
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
        return voicelife::Result<std::vector<ScheduleRule>>::Success(rules);
    }

    /**
     * @brief 按标识读取规则。
     * @param id 规则标识。
     * @return 规则或错误。
     */
    [[nodiscard]] voicelife::Result<ScheduleRule> FindById(voicelife::schedule::ScheduleRuleId id) const override {
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
        const auto found = FindById(id);
        if (!found.ok()) return found.status;
        ScheduleRule cancelled = *found.value;
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
        .limit = 10,
        .offset = 0,
    });
    Check(queried.status.ok() && queried.total == 1 && queried.rules.size() == 1 &&
              queried.rules.front().upcoming_occurrences.size() == 3 && queried.rules.front().exceptions.size() == 1,
          "查询周期规则必须返回未来发生时间和例外");

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
    return 0;
}
