#include "voicelife/mcp/schedule_rule_mcp_tools.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "support/in_memory_schedule_repository.h"
#include "support/test_support.h"
#include "voicelife/mcp/mcp_server.h"
#include "voicelife/schedule/calendar.h"
#include "voicelife/schedule/schedule_exception_repository.h"
#include "voicelife/schedule/schedule_rule_repository.h"
#include "voicelife/schedule/schedule_rule_service.h"

using voicelife::ErrorCode;
using voicelife::ToolCall;
using voicelife::mcp::McpServer;
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

/** @brief 测试用的内存例外仓储。 */
class FakeExceptionRepository final : public voicelife::schedule::ScheduleExceptionRepository {
   public:
    /**
     * @brief 插入或更新例外。
     * @param exception 待写入例外。
     * @return 保存后的例外。
     */
    voicelife::Result<ScheduleException> Upsert(const ScheduleException& exception) override {
        for (ScheduleException& existing : exceptions) {
            if (existing.rule_id == exception.rule_id &&
                existing.original_start_time == exception.original_start_time) {
                existing = exception;
                return voicelife::Result<ScheduleException>::Success(existing);
            }
        }
        ScheduleException stored = exception;
        stored.id = next_id_++;
        exceptions.push_back(stored);
        return voicelife::Result<ScheduleException>::Success(std::move(stored));
    }

    /**
     * @brief 查询规则例外。
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
     * @brief 按规则和时间查询例外。
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
        std::vector<ScheduleException> kept;
        for (const ScheduleException& exception : exceptions) {
            if (exception.rule_id != rule_id || exception.original_start_time <= after) kept.push_back(exception);
        }
        exceptions = std::move(kept);
        return voicelife::Status::Ok();
    }

    std::vector<ScheduleException> exceptions;
    int64_t next_id_ = 700;
};

/** @brief 测试用的内存规则仓储。 */
class FakeRuleRepository final : public voicelife::schedule::ScheduleRuleRepository {
   public:
    /**
     * @brief 使用日程和例外仓储构造规则仓储。
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
     * @brief 更新规则。
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
     * @param first_instance 首条实例。
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
     * @brief 更新规则并重建实例。
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
     * @brief 取消规则和实例。
     * @param id 规则标识。
     * @param cancelled_instance_count 输出取消实例数。
     * @return 成功状态。
     */
    voicelife::Status CancelRuleAndInstances(voicelife::schedule::ScheduleRuleId id,
                                             int64_t& cancelled_instance_count) override {
        const auto loaded = FindById(id);
        if (!loaded.ok()) return loaded.status;
        ScheduleRule cancelled = *loaded.value;
        cancelled.status = ScheduleStatus::kCancelled;
        const voicelife::Status updated = Update(cancelled);
        if (!updated.ok()) return updated;
        cancelled_instance_count = 0;
        voicelife::schedule::QueryScheduleCommand query;
        query.rule_id = id;
        query.status = ScheduleStatusFilter::kAll;
        query.limit = 100;
        const auto schedules = schedules_.Find(query);
        if (!schedules.ok()) return schedules.status;
        for (Schedule schedule : *schedules.value) {
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
     * @brief 创建下一条实例。
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
    int64_t next_id_ = 600;

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
    McpServer server;
    Check(voicelife::mcp::RegisterScheduleRuleMcpTools(server, service).ok(), "周期规则 MCP 工具应注册成功");

    const auto listed = server.list_tools();
    Check(listed.total == 7 && listed.tools.size() == 7, "周期规则 MCP 工具应注册七个稳定工具");
    Check(listed.tools[0].name == "schedule_rule.create" && listed.tools[1].name == "schedule_rule.query" &&
              listed.tools[2].name == "schedule_occurrence.skip" && listed.tools[3].name == "schedule_rule.update" &&
              listed.tools[4].name == "schedule_rule.cancel" && listed.tools[5].name == "schedule_occurrence.update" &&
              listed.tools[6].name == "schedule_rule.generate_next",
          "周期规则 MCP 工具应保持稳定注册顺序");

    const auto created = server.call({
        .request_id = "rule-create",
        .name = "schedule_rule.create",
        .arguments =
            {
                {"event", std::string("规则创建")},
                {"freq_type", std::string("daily")},
                {"start_time", std::string("09:00:00")},
            },
    });
    Check(created.status.ok() && created.output.IsObject(), "schedule_rule.create 应返回结构化成功结果");

    const auto queried = server.call({
        .request_id = "rule-query",
        .name = "schedule_rule.query",
        .arguments = {{"status", std::string("all")}},
    });
    Check(queried.status.ok() && queried.output.IsObject(), "schedule_rule.query 应返回规则查询结果");

    const auto skipped = server.call({
        .request_id = "occurrence-skip",
        .name = "schedule_occurrence.skip",
        .arguments =
            {
                {"rule_id", int64_t{600}},
                {"original_start_time", int64_t{UtcAtLocal(2099, 1, 2, 9)}},
            },
    });
    Check(skipped.status.ok() && skipped.output.IsObject(), "schedule_occurrence.skip 应返回例外对象");

    const auto updated = server.call({
        .request_id = "rule-update",
        .name = "schedule_rule.update",
        .arguments =
            {
                {"rule_id", int64_t{600}},
                {"event", std::string("规则更新")},
            },
    });
    Check(updated.status.ok() && updated.output.IsObject(), "schedule_rule.update 应返回规则更新结果");

    const auto generated = server.call({
        .request_id = "rule-generate",
        .name = "schedule_rule.generate_next",
        .arguments = {{"rule_id", int64_t{600}}},
    });
    Check(generated.status.ok() && generated.output.IsObject(), "schedule_rule.generate_next 应返回下一实例");

    const auto cancelled = server.call({
        .request_id = "rule-cancel",
        .name = "schedule_rule.cancel",
        .arguments = {{"rule_id", int64_t{600}}},
    });
    Check(cancelled.status.ok() && cancelled.output.IsObject(), "schedule_rule.cancel 应返回取消结果");

    // —— 失败路径与可选字段分支覆盖 ——

    // create：非法开始时间。
    const auto bad_start_time = server.call({
        .request_id = "rule-create-bad-time",
        .name = "schedule_rule.create",
        .arguments =
            {
                {"event", std::string("坏时间")},
                {"freq_type", std::string("daily")},
                {"start_time", std::string("9点")},
            },
    });
    Check(!bad_start_time.status.ok(), "非法开始时间应返回参数错误");

    // create：非法周期间隔（字段校验失败）。
    const auto bad_interval = server.call({
        .request_id = "rule-create-bad-interval",
        .name = "schedule_rule.create",
        .arguments =
            {
                {"event", std::string("坏间隔")},
                {"freq_type", std::string("daily")},
                {"start_time", std::string("09:00:00")},
                {"interval_val", int64_t{0}},
            },
    });
    Check(!bad_interval.status.ok(), "非法周期间隔应返回参数错误");

    // create：失效日期早于当前，无法计算首个发生时间。
    const auto no_first = server.call({
        .request_id = "rule-create-no-first",
        .name = "schedule_rule.create",
        .arguments =
            {
                {"event", std::string("无发生")},
                {"freq_type", std::string("daily")},
                {"start_time", std::string("09:00:00")},
                {"end_date", std::string("2020-01-01")},
            },
    });
    Check(!no_first.status.ok(), "无法计算首个发生时间应返回错误");

    // create：携带全部可选字段（weekly），命中解析与输出分支。
    const auto full_create = server.call({
        .request_id = "rule-create-full",
        .name = "schedule_rule.create",
        .arguments =
            {
                {"event", std::string("全字段规则")},
                {"freq_type", std::string("weekly")},
                {"start_time", std::string("09:30:00")},
                {"end_time", std::string("10:30:00")},
                {"location", std::string("A座")},
                {"notes", std::string("备注")},
                {"interval_val", int64_t{2}},
                {"weekdays_mask", int64_t{3}},
                {"monthly_mode", std::string("specific_day")},
                {"day_of_month", int64_t{15}},
                {"month_of_year", int64_t{6}},
                {"end_date", std::string("2099-12-31")},
                {"ignore_conflict", bool{true}},
            },
    });
    Check(full_create.status.ok() && full_create.output.IsObject(), "全可选字段的 create 应成功");

    // create：monthly/yearly/非法频率与 last_day/非法月模式，命中解析分支。
    const auto monthly_create = server.call({
        .request_id = "rule-create-monthly",
        .name = "schedule_rule.create",
        .arguments =
            {
                {"event", std::string("月度规则")},
                {"freq_type", std::string("monthly")},
                {"start_time", std::string("09:00:00")},
                {"monthly_mode", std::string("last_day")},
            },
    });
    Check(monthly_create.status.ok(), "月度规则 create 应成功");

    const auto yearly_create = server.call({
        .request_id = "rule-create-yearly",
        .name = "schedule_rule.create",
        .arguments =
            {
                {"event", std::string("年度规则")},
                {"freq_type", std::string("yearly")},
                {"start_time", std::string("09:00:00")},
                {"month_of_year", int64_t{6}},
                {"day_of_month", int64_t{15}},
            },
    });
    Check(yearly_create.status.ok(), "年度规则 create 应成功");

    const auto bad_freq = server.call({
        .request_id = "rule-create-bad-freq",
        .name = "schedule_rule.create",
        .arguments =
            {
                {"event", std::string("非法频率")},
                {"freq_type", std::string("hourly")},
                {"start_time", std::string("09:00:00")},
            },
    });
    Check(bad_freq.status.ok(), "非法频率应回退为 daily 并成功创建");

    const auto bad_mode = server.call({
        .request_id = "rule-create-bad-mode",
        .name = "schedule_rule.create",
        .arguments =
            {
                {"event", std::string("非法月模式")},
                {"freq_type", std::string("daily")},
                {"start_time", std::string("09:00:00")},
                {"monthly_mode", std::string("invalid")},
                {"ignore_conflict", bool{true}},
            },
    });
    Check(bad_mode.status.ok(), "非法月模式应被忽略并成功创建");

    // update：规则标识非法。
    const auto bad_rule_id = server.call({
        .request_id = "rule-update-bad-id",
        .name = "schedule_rule.update",
        .arguments = {{"rule_id", int64_t{0}}},
    });
    Check(!bad_rule_id.status.ok(), "非法规则标识的 update 应返回错误");

    // update：携带全部可选字段（含 occurrence_count），命中解析分支后由字段校验拒绝。
    const auto full_update_rejected = server.call({
        .request_id = "rule-update-full-rejected",
        .name = "schedule_rule.update",
        .arguments =
            {
                {"rule_id", int64_t{600}},
                {"event", std::string("改")},
                {"location", std::string("L")},
                {"notes", std::string("N")},
                {"freq_type", std::string("daily")},
                {"interval_val", int64_t{2}},
                {"weekdays_mask", int64_t{1}},
                {"monthly_mode", std::string("specific_day")},
                {"day_of_month", int64_t{15}},
                {"month_of_year", int64_t{6}},
                {"start_time", std::string("08:00:00")},
                {"end_time", std::string("09:00:00")},
                {"end_date", std::string("2099-12-31")},
                {"occurrence_count", int64_t{3}},
            },
    });
    Check(!full_update_rejected.status.ok(), "携带 occurrence_count 的 update 应被字段校验拒绝");

    // occurrence.update：携带全部可选字段（未物化实例）。
    const auto occurrence_updated = server.call({
        .request_id = "occ-update-full",
        .name = "schedule_occurrence.update",
        .arguments =
            {
                {"rule_id", int64_t{600}},
                {"original_start_time", int64_t{UtcAtLocal(2099, 1, 5, 9)}},
                {"event", std::string("改事件")},
                {"start_time", int64_t{UtcAtLocal(2099, 1, 5, 10)}},
                {"end_time", int64_t{UtcAtLocal(2099, 1, 5, 11)}},
                {"location", std::string("L")},
                {"notes", std::string("N")},
                {"ignore_conflict", bool{false}},
            },
    });
    Check(occurrence_updated.status.ok() && occurrence_updated.output.IsObject(),
          "occurrence.update 应返回含例外的成功结果");

    // occurrence.update：未提供任何修改字段。
    const auto occurrence_no_field = server.call({
        .request_id = "occ-update-no-field",
        .name = "schedule_occurrence.update",
        .arguments =
            {
                {"rule_id", int64_t{600}},
                {"original_start_time", int64_t{UtcAtLocal(2099, 1, 6, 9)}},
            },
    });
    Check(!occurrence_no_field.status.ok(), "未提供修改字段的 occurrence.update 应返回错误");

    // occurrence.update：非法规则标识。
    const auto occurrence_bad_id = server.call({
        .request_id = "occ-update-bad-id",
        .name = "schedule_occurrence.update",
        .arguments =
            {
                {"rule_id", int64_t{0}},
                {"original_start_time", int64_t{UtcAtLocal(2099, 1, 6, 9)}},
                {"event", std::string("x")},
            },
    });
    Check(!occurrence_bad_id.status.ok(), "非法规则标识的 occurrence.update 应返回错误");

    // skip：非法规则标识。
    const auto skip_bad_id = server.call({
        .request_id = "occ-skip-bad-id",
        .name = "schedule_occurrence.skip",
        .arguments = {{"rule_id", int64_t{0}}, {"original_start_time", int64_t{UtcAtLocal(2099, 1, 7, 9)}}},
    });
    Check(!skip_bad_id.status.ok(), "非法规则标识的 skip 应返回错误");

    // cancel：非法规则标识与不存在规则。
    const auto cancel_bad_id = server.call({
        .request_id = "rule-cancel-bad-id",
        .name = "schedule_rule.cancel",
        .arguments = {{"rule_id", int64_t{0}}},
    });
    Check(!cancel_bad_id.status.ok(), "非法规则标识的 cancel 应返回错误");
    const auto cancel_missing = server.call({
        .request_id = "rule-cancel-missing",
        .name = "schedule_rule.cancel",
        .arguments = {{"rule_id", int64_t{999999}}},
    });
    Check(!cancel_missing.status.ok(), "取消不存在规则应返回错误");

    // generate_next：非法规则标识与不存在规则。
    const auto generate_bad_id = server.call({
        .request_id = "rule-generate-bad-id",
        .name = "schedule_rule.generate_next",
        .arguments = {{"rule_id", int64_t{0}}},
    });
    Check(!generate_bad_id.status.ok(), "非法规则标识的 generate_next 应返回错误");
    const auto generate_missing = server.call({
        .request_id = "rule-generate-missing",
        .name = "schedule_rule.generate_next",
        .arguments = {{"rule_id", int64_t{999999}}},
    });
    Check(!generate_missing.status.ok(), "生成不存在规则的下一条实例应返回错误");

    // 预置全字段规则与例外，命中 RuleOutput/ExceptionOutput 的可选字段分支。
    ScheduleRule monthly_rule;
    monthly_rule.id = 610;
    monthly_rule.event = "月度全字段";
    monthly_rule.location = "月度地点";
    monthly_rule.notes = "月度备注";
    monthly_rule.freq_type = Frequency::kMonthly;
    monthly_rule.interval_val = 1;
    monthly_rule.weekdays_mask = 7;
    monthly_rule.day_of_month = 20;
    monthly_rule.month_of_year = 5;
    monthly_rule.monthly_mode = MonthlyMode::kLastDay;
    monthly_rule.start_time = LocalTime{9, 0, 0};
    monthly_rule.end_time = LocalTime{10, 0, 0};
    monthly_rule.start_date = LocalDate{2099, 1, 1};
    monthly_rule.end_date = LocalDate{2099, 12, 31};
    monthly_rule.occurrence_count = 5;
    monthly_rule.status = ScheduleStatus::kActive;
    rules.rules.push_back(monthly_rule);

    ScheduleRule yearly_rule;
    yearly_rule.id = 611;
    yearly_rule.event = "年度规则";
    yearly_rule.freq_type = Frequency::kYearly;
    yearly_rule.interval_val = 1;
    yearly_rule.month_of_year = 6;
    yearly_rule.day_of_month = 15;
    yearly_rule.start_time = LocalTime{12, 0, 0};
    yearly_rule.start_date = LocalDate{2099, 1, 1};
    yearly_rule.status = ScheduleStatus::kActive;
    rules.rules.push_back(yearly_rule);

    ScheduleException full_exception;
    full_exception.id = 800;
    full_exception.rule_id = 610;
    full_exception.original_start_time = DateTime{std::chrono::seconds{UtcAtLocal(2099, 2, 28, 9)}};
    full_exception.schedule_id = 900;
    full_exception.type = ExceptionType::kModify;
    full_exception.override_start_time = DateTime{std::chrono::seconds{UtcAtLocal(2099, 2, 28, 10)}};
    full_exception.override_end_time = DateTime{std::chrono::seconds{UtcAtLocal(2099, 2, 28, 11)}};
    full_exception.override_event = "覆盖事件";
    exceptions.exceptions.push_back(full_exception);

    const auto filtered_query = server.call({
        .request_id = "rule-query-filtered",
        .name = "schedule_rule.query",
        .arguments = {{"rule_id", int64_t{610}}, {"keyword", std::string("月度")}},
    });
    Check(filtered_query.status.ok() && filtered_query.output.IsObject(), "带筛选条件的 query 应返回全字段规则");

    const auto active_query = server.call({
        .request_id = "rule-query-active",
        .name = "schedule_rule.query",
        .arguments = {},
    });
    Check(active_query.status.ok() && active_query.output.IsObject(), "默认 active 状态的 query 应返回结果");
    return 0;
}
