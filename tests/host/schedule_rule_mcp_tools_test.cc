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
    return 0;
}
