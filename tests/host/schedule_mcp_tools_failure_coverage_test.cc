#include "voicelife/mcp/schedule_mcp_tools.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "support/in_memory_schedule_repository.h"
#include "support/test_support.h"
#include "voicelife/contracts/json.h"
#include "voicelife/mcp/mcp_server.h"
#include "voicelife/schedule/schedule_exception_repository.h"
#include "voicelife/schedule/schedule_rule_repository.h"
#include "voicelife/schedule/schedule_rule_service.h"
#include "voicelife/schedule/schedule_service.h"

using voicelife::ErrorCode;
using voicelife::JsonValue;
using voicelife::Status;
using voicelife::ToolResult;
using voicelife::mcp::McpServer;
using voicelife::schedule::DateTime;
using voicelife::schedule::ExceptionType;
using voicelife::schedule::Frequency;
using voicelife::schedule::LocalDate;
using voicelife::schedule::LocalTime;
using voicelife::schedule::Schedule;
using voicelife::schedule::ScheduleException;
using voicelife::schedule::ScheduleRule;
using voicelife::schedule::ScheduleRuleId;
using voicelife::schedule::ScheduleRuleService;
using voicelife::schedule::ScheduleService;
using voicelife::schedule::ScheduleStatus;
using voicelife::schedule::ScheduleStatusFilter;
using voicelife::test::Check;
using voicelife::test::InMemoryScheduleRepository;

namespace {

/** @brief 测试用的可注入失败例外仓储。 */
class FakeExceptionRepository final : public voicelife::schedule::ScheduleExceptionRepository {
   public:
    /** @brief 插入或更新例外。 @param exception 待保存例外。 @return 保存后的例外。 */
    voicelife::Result<ScheduleException> Upsert(const ScheduleException& exception) override {
        if (next_upsert_failure.has_value()) {
            Status failure = std::move(*next_upsert_failure);
            next_upsert_failure.reset();
            return voicelife::Result<ScheduleException>::Failure(failure.code, failure.message);
        }
        for (ScheduleException& existing : exceptions) {
            if (existing.rule_id == exception.rule_id &&
                existing.original_start_time == exception.original_start_time) {
                existing = exception;
                return voicelife::Result<ScheduleException>::Success(existing);
            }
        }
        ScheduleException stored = exception;
        stored.id = next_id++;
        exceptions.push_back(stored);
        return voicelife::Result<ScheduleException>::Success(std::move(stored));
    }

    /** @brief 查询规则例外。 @param rule_id 规则标识。 @return 例外集合。 */
    [[nodiscard]] voicelife::Result<std::vector<ScheduleException>> FindByRule(
        voicelife::schedule::ScheduleRuleId rule_id) const override {
        if (next_find_failure.has_value()) {
            Status failure = std::move(*next_find_failure);
            next_find_failure.reset();
            return voicelife::Result<std::vector<ScheduleException>>::Failure(failure.code, failure.message);
        }
        std::vector<ScheduleException> matched;
        for (const ScheduleException& exception : exceptions) {
            if (exception.rule_id == rule_id) matched.push_back(exception);
        }
        return voicelife::Result<std::vector<ScheduleException>>::Success(std::move(matched));
    }

    /** @brief 按规则和原始时间查找例外。 @param rule_id 规则标识。 @param original_start_time 原始时间。 @return 例外。 */
    [[nodiscard]] voicelife::Result<std::optional<ScheduleException>> FindByRuleAndTime(
        voicelife::schedule::ScheduleRuleId rule_id, DateTime original_start_time) const override {
        for (const ScheduleException& exception : exceptions) {
            if (exception.rule_id == rule_id && exception.original_start_time == original_start_time) {
                return voicelife::Result<std::optional<ScheduleException>>::Success(exception);
            }
        }
        return voicelife::Result<std::optional<ScheduleException>>::Success(std::nullopt);
    }

    /** @brief 删除未来例外。 @param rule_id 规则标识。 @param after 边界时间。 @return 成功状态。 */
    voicelife::Status DeleteFuture(voicelife::schedule::ScheduleRuleId rule_id, DateTime after) override {
        (void)rule_id;
        (void)after;
        return voicelife::Status::Ok();
    }

    std::vector<ScheduleException> exceptions;
    std::optional<Status> next_upsert_failure;
    mutable std::optional<Status> next_find_failure;
    int64_t next_id = 700;
};

/** @brief 测试用的可注入失败规则仓储。 */
class FakeRuleRepository final : public voicelife::schedule::ScheduleRuleRepository {
   public:
    /**
     * @brief 使用日程和例外仓储构造规则仓储。
     * @param schedules 日程仓储。
     * @param exceptions 例外仓储。
     */
    FakeRuleRepository(InMemoryScheduleRepository& schedules, FakeExceptionRepository& exceptions)
        : schedules_(schedules), exceptions_(exceptions) {}

    /** @brief 插入规则。 @param rule 待保存规则。 @return 保存后的规则。 */
    voicelife::Result<ScheduleRule> Insert(const ScheduleRule& rule) override {
        if (next_insert_failure.has_value()) {
            Status failure = std::move(*next_insert_failure);
            next_insert_failure.reset();
            return voicelife::Result<ScheduleRule>::Failure(failure.code, failure.message);
        }
        ScheduleRule stored = rule;
        stored.id = next_id++;
        rules.push_back(stored);
        return voicelife::Result<ScheduleRule>::Success(std::move(stored));
    }

    /** @brief 更新规则。 @param rule 待更新规则。 @return 更新状态。 */
    voicelife::Status Update(const ScheduleRule& rule) override {
        if (next_update_failure.has_value()) {
            Status failure = std::move(*next_update_failure);
            next_update_failure.reset();
            return failure;
        }
        for (ScheduleRule& existing : rules) {
            if (existing.id == rule.id) {
                existing = rule;
                return voicelife::Status::Ok();
            }
        }
        return voicelife::Status::Error(ErrorCode::kNotFound, "规则不存在");
    }

    /** @brief 查询全部规则。 @return 规则集合。 */
    [[nodiscard]] voicelife::Result<std::vector<ScheduleRule>> FindAll() const override {
        if (next_find_all_failure.has_value()) {
            Status failure = std::move(*next_find_all_failure);
            next_find_all_failure.reset();
            return voicelife::Result<std::vector<ScheduleRule>>::Failure(failure.code, failure.message);
        }
        return voicelife::Result<std::vector<ScheduleRule>>::Success(rules);
    }

    /** @brief 按标识读取规则。 @param id 规则标识。 @return 规则或错误。 */
    [[nodiscard]] voicelife::Result<ScheduleRule> FindById(voicelife::schedule::ScheduleRuleId id) const override {
        if (next_find_by_id_failure.has_value()) {
            Status failure = std::move(*next_find_by_id_failure);
            next_find_by_id_failure.reset();
            return voicelife::Result<ScheduleRule>::Failure(failure.code, failure.message);
        }
        for (const ScheduleRule& rule : rules) {
            if (rule.id == id) return voicelife::Result<ScheduleRule>::Success(rule);
        }
        return voicelife::Result<ScheduleRule>::Failure(ErrorCode::kNotFound, "规则不存在");
    }

    /** @brief 创建规则和首条实例。 @param rule 规则。 @param first_instance 首条实例。 @return 创建后的规则。 */
    voicelife::Result<ScheduleRule> CreateWithFirstInstance(
        const ScheduleRule& rule, const std::optional<Schedule>& first_instance) override {
        const auto created = Insert(rule);
        if (!created.ok()) return created;
        if (first_instance.has_value()) {
            Schedule instance = *first_instance;
            instance.rule_id = created.value->id;
            const auto saved = schedules_.Insert(instance);
            if (!saved.ok()) return voicelife::Result<ScheduleRule>::Failure(saved.status.code, saved.status.message);
        }
        return created;
    }

    /** @brief 更新规则并重建实例。 @param rule 规则。 @param first_instance 首条实例。 @return 更新后的规则。 */
    voicelife::Result<ScheduleRule> UpdateAndRebuild(
        const ScheduleRule& rule, const std::optional<Schedule>& first_instance) override {
        const Status updated = Update(rule);
        if (!updated.ok()) return voicelife::Result<ScheduleRule>::Failure(updated.code, updated.message);
        if (first_instance.has_value()) {
            Schedule instance = *first_instance;
            instance.rule_id = rule.id;
            const auto saved = schedules_.Insert(instance);
            if (!saved.ok()) return voicelife::Result<ScheduleRule>::Failure(saved.status.code, saved.status.message);
        }
        return FindById(rule.id);
    }

    /** @brief 取消规则和实例。 @param id 规则标识。 @param cancelled_instance_count 输出实例数。 @return 状态。 */
    voicelife::Status CancelRuleAndInstances(voicelife::schedule::ScheduleRuleId id,
                                             int64_t& cancelled_instance_count) override {
        if (next_cancel_failure.has_value()) {
            Status failure = std::move(*next_cancel_failure);
            next_cancel_failure.reset();
            return failure;
        }
        const auto loaded = FindById(id);
        if (!loaded.ok()) return loaded.status;
        ScheduleRule cancelled = *loaded.value;
        cancelled.status = ScheduleStatus::kCancelled;
        const Status updated = Update(cancelled);
        if (!updated.ok()) return updated;
        cancelled_instance_count = 0;
        return Status::Ok();
    }

    /** @brief 创建下一条实例。 @param schedule 实例。 @param linked_exception 关联例外。 @return 实例。 */
    voicelife::Result<Schedule> CreateNextInstance(
        const Schedule& schedule, const std::optional<ScheduleException>& linked_exception) override {
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
    std::optional<Status> next_insert_failure;
    std::optional<Status> next_update_failure;
    std::optional<Status> next_cancel_failure;
    mutable std::optional<Status> next_find_all_failure;
    mutable std::optional<Status> next_find_by_id_failure;
    int64_t next_id = 600;

   private:
    InMemoryScheduleRepository& schedules_;
    FakeExceptionRepository& exceptions_;
};

/** @brief 从工具输出对象中读取字符串字段。 @param result 工具结果。 @param key 字段名。 @return 字段值或空。 */
std::string OutputString(const ToolResult& result, const std::string& key) {
    if (!result.output.IsObject()) return {};
    for (const auto& field : *result.output.object) {
        if (field.first == key && field.second->IsString()) return field.second->string;
    }
    return {};
}

/** @brief 构造每日周期 repeat 对象。 @return repeat JSON 对象。 */
JsonValue DailyRepeat() {
    return JsonValue::Object({
        {"freq_type", JsonValue::String("daily")},
        {"start_date", JsonValue::String("2099-01-01")},
        {"start_time", JsonValue::String("09:00:00")},
    });
}

/** @brief 构造测试规则。 @param id 规则标识。 @return 周期规则。 */
ScheduleRule Rule(ScheduleRuleId id) {
    ScheduleRule rule;
    rule.id = id;
    rule.event = "每日站会";
    rule.freq_type = Frequency::kDaily;
    rule.interval_val = 1;
    rule.start_time = LocalTime{9, 0, 0};
    rule.start_date = LocalDate{2099, 1, 1};
    rule.status = ScheduleStatus::kActive;
    return rule;
}

/** @brief 按东八区本地时间构造 Unix 秒。 @param day 日期。 @return Unix 秒。 */
int64_t UtcAtLocalDay(int day) {
    return voicelife::schedule::DaysFromCivil(2099, 1, day) * 86400 + 9 * 3600 - 8 * 3600;
}

}  // namespace

/**
 * @brief 执行 MCP 日程工具失败路径新增覆盖测试。
 * @return 全部断言通过时返回 0。
 */
int main() {
    InMemoryScheduleRepository schedules;
    FakeExceptionRepository exceptions;
    FakeRuleRepository rules(schedules, exceptions);
    ScheduleRuleService rule_service(rules, exceptions, schedules);
    ScheduleService service(schedules);
    McpServer server;
    Check(voicelife::mcp::RegisterScheduleMcpTools(server, service, rule_service).ok(), "日程 MCP 工具应注册成功");

    schedules.FailNextFindOverlapping(Status::Error(ErrorCode::kUnavailable, "候选查询失败"));
    const auto create_overlap_failed = server.call({
        .request_id = "create-overlap-failed",
        .name = "schedule.create",
        .arguments = {{"event", std::string("失败日程")}, {"start_time", std::string("2030-01-01 09:00:00")}},
    });
    Check(OutputString(create_overlap_failed, "status") == "failure", "候选查询失败应返回失败输出");

    rules.next_insert_failure = Status::Error(ErrorCode::kUnavailable, "规则写入失败");
    const auto create_rule_failed = server.call({
        .request_id = "create-rule-failed",
        .name = "schedule.create",
        .arguments = {{"event", std::string("周期失败")}, {"repeat", DailyRepeat()}},
    });
    Check(OutputString(create_rule_failed, "status") == "failure", "周期规则创建非冲突失败应返回 failure");

    schedules.FailNextFind(Status::Error(ErrorCode::kUnavailable, "查询失败"));
    const auto query_find_failed = server.call({
        .request_id = "query-find-failed",
        .name = "schedule.query",
        .arguments = {{"status", std::string("all")}},
    });
    Check(OutputString(query_find_failed, "status") == "failure", "查询日程失败应返回 failure");

    schedules.FailNextCount(Status::Error(ErrorCode::kUnavailable, "计数失败"));
    const auto query_count_failed = server.call({
        .request_id = "query-count-failed",
        .name = "schedule.query",
        .arguments = {{"status", std::string("all")}},
    });
    Check(OutputString(query_count_failed, "status") == "failure", "查询计数失败应返回 failure");

    rules.rules.push_back(Rule(600));
    exceptions.next_find_failure = Status::Error(ErrorCode::kUnavailable, "例外查询失败");
    const auto query_rule_failed = server.call({
        .request_id = "query-rule-failed",
        .name = "schedule.query",
        .arguments = {{"status", std::string("all")}},
    });
    Check(OutputString(query_rule_failed, "status") == "failure", "周期规则查询失败应返回 failure");

    schedules.Reset({Schedule{.id = 1,
                              .event = "可取消日程",
                              .start_time = DateTime{std::chrono::seconds{1'900'000'000}},
                              .status = ScheduleStatus::kActive}});
    schedules.FailNextFind(Status::Error(ErrorCode::kUnavailable, "删除读取失败"));
    const auto delete_load_failed = server.call({
        .request_id = "delete-load-failed",
        .name = "schedule.delete",
        .arguments = {{"schedule_id", int64_t{1}}},
    });
    Check(OutputString(delete_load_failed, "status") == "failure", "删除前读取失败应返回 failure");

    rules.next_cancel_failure = Status::Error(ErrorCode::kUnavailable, "取消规则失败");
    const auto delete_rule_failed = server.call({
        .request_id = "delete-rule-failed",
        .name = "schedule.delete",
        .arguments = {{"rule_id", int64_t{600}}},
    });
    Check(OutputString(delete_rule_failed, "status") == "failure", "取消周期规则失败应返回 failure");

    exceptions.next_upsert_failure = Status::Error(ErrorCode::kUnavailable, "跳过失败");
    const auto delete_occurrence_failed = server.call({
        .request_id = "delete-occurrence-failed",
        .name = "schedule.delete",
        .arguments = {{"rule_id", int64_t{600}}, {"original_start_time", std::string("2099-01-03 09:00:00")}},
    });
    Check(OutputString(delete_occurrence_failed, "status") == "failure", "删除未来单次失败应返回 failure");

    exceptions.next_upsert_failure = Status::Error(ErrorCode::kUnavailable, "单次更新失败");
    const auto update_occurrence_failed = server.call({
        .request_id = "update-occurrence-failed",
        .name = "schedule.update",
        .arguments = {{"rule_id", int64_t{600}},
                      {"original_start_time", std::string("2099-01-04 09:00:00")},
                      {"event", std::string("失败更新")}},
    });
    Check(OutputString(update_occurrence_failed, "status") == "failure", "更新未来单次失败应返回 failure");

    schedules.Reset({Schedule{.id = 2,
                              .event = "冲突更新源",
                              .start_time = DateTime{std::chrono::seconds{1'900'000'000}},
                              .end_time = DateTime{std::chrono::seconds{1'900'003'600}},
                              .status = ScheduleStatus::kActive},
                     Schedule{.id = 3,
                              .event = "冲突目标",
                              .start_time = DateTime{std::chrono::seconds{1'900'001'800}},
                              .end_time = DateTime{std::chrono::seconds{1'900'004'000}},
                              .status = ScheduleStatus::kActive}});
    const auto update_conflict = server.call({
        .request_id = "update-conflict",
        .name = "schedule.update",
        .arguments = {{"schedule_id", int64_t{2}}, {"start_time", std::string("2030-03-17 18:43:20")}},
    });
    Check(OutputString(update_conflict, "status") == "conflict", "一次性日程更新冲突应返回 conflict");

    const auto ignored = UtcAtLocalDay(5);
    (void)ignored;
    return 0;
}
