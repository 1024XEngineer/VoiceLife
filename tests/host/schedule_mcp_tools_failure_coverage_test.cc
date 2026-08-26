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
#include "voicelife/mcp/schedule_mcp_tools.h"
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

    /** @brief 按规则和原始时间查找例外。 @param rule_id 规则标识。 @param original_start_time 原始时间。 @return 例外。
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
    voicelife::Result<ScheduleRule> CreateWithFirstInstance(const ScheduleRule& rule,
                                                            const std::optional<Schedule>& first_instance) override {
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
    voicelife::Result<ScheduleRule> UpdateAndRebuild(const ScheduleRule& rule,
                                                     const std::optional<Schedule>& first_instance) override {
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

/** @brief 测试用的可注入失败日程仓储。 */
class FailingScheduleRepository final : public voicelife::schedule::ScheduleRepository {
   public:
    /** @brief 插入日程。 @param schedule 待保存日程。 @return 保存后的日程或注入错误。 */
    voicelife::Result<Schedule> Insert(const Schedule& schedule) override {
        if (insert_failure.has_value())
            return voicelife::Result<Schedule>::Failure(insert_failure->code, insert_failure->message);
        Schedule stored = schedule;
        stored.id = next_id++;
        schedules.push_back(stored);
        return voicelife::Result<Schedule>::Success(std::move(stored));
    }

    /** @brief 更新日程。 @param schedule 待更新日程。 @return 更新状态或注入错误。 */
    Status Update(const Schedule& schedule) override {
        if (update_failure.has_value()) return *update_failure;
        for (Schedule& existing : schedules) {
            if (existing.id == schedule.id) {
                existing = schedule;
                return Status::Ok();
            }
        }
        return Status::Error(ErrorCode::kNotFound, "日程不存在");
    }

    /** @brief 取消日程。 @param id 日程标识。 @return 删除状态或注入错误。 */
    Status Delete(voicelife::schedule::ScheduleId id) override {
        if (delete_failure.has_value()) return *delete_failure;
        for (Schedule& existing : schedules) {
            if (existing.id == id) {
                existing.status = ScheduleStatus::kCancelled;
                return Status::Ok();
            }
        }
        return Status::Error(ErrorCode::kNotFound, "日程不存在");
    }

    /** @brief 按标识查找日程。 @param id 日程标识。 @return 日程或注入错误。 */
    [[nodiscard]] voicelife::Result<Schedule> FindById(voicelife::schedule::ScheduleId id) const override {
        if (find_by_id_failure.has_value()) {
            return voicelife::Result<Schedule>::Failure(find_by_id_failure->code, find_by_id_failure->message);
        }
        for (const Schedule& schedule : schedules) {
            if (schedule.id == id) return voicelife::Result<Schedule>::Success(schedule);
        }
        return voicelife::Result<Schedule>::Failure(ErrorCode::kNotFound, "日程不存在");
    }

    /** @brief 按条件查询日程。 @param query 查询条件。 @return 日程集合。 */
    [[nodiscard]] voicelife::Result<std::vector<Schedule>> Find(
        const voicelife::schedule::QueryScheduleCommand& query) const override {
        if (find_failure.has_value())
            return voicelife::Result<std::vector<Schedule>>::Failure(find_failure->code, find_failure->message);
        std::vector<Schedule> matched;
        for (const Schedule& schedule : schedules) {
            if (query.schedule_id.has_value() && schedule.id != *query.schedule_id) continue;
            matched.push_back(schedule);
        }
        return voicelife::Result<std::vector<Schedule>>::Success(std::move(matched));
    }

    /** @brief 统计日程。 @param query 查询条件。 @return 命中总数。 */
    [[nodiscard]] voicelife::Result<int64_t> Count(
        const voicelife::schedule::QueryScheduleCommand& query) const override {
        (void)query;
        return voicelife::Result<int64_t>::Success(static_cast<int64_t>(schedules.size()));
    }

    /**
     * @brief 查询重叠日程。
     * @param start 起始时间。
     * @param end 结束时间。
     * @param exclude_id 排除日程标识。
     * @return 重叠日程集合或注入错误。
     */
    [[nodiscard]] voicelife::Result<std::vector<Schedule>> FindOverlapping(
        DateTime start, DateTime end, std::optional<voicelife::schedule::ScheduleId> exclude_id) const override {
        (void)start;
        (void)end;
        (void)exclude_id;
        if (overlap_failure.has_value()) {
            return voicelife::Result<std::vector<Schedule>>::Failure(overlap_failure->code, overlap_failure->message);
        }
        return voicelife::Result<std::vector<Schedule>>::Success(std::vector<Schedule>{});
    }

    /** @brief 查询全部日程。 @return 全部日程。 */
    [[nodiscard]] voicelife::Result<std::vector<Schedule>> FindAll() const override {
        return voicelife::Result<std::vector<Schedule>>::Success(schedules);
    }

    std::vector<Schedule> schedules;
    std::optional<Status> insert_failure;
    std::optional<Status> update_failure;
    std::optional<Status> delete_failure;
    mutable std::optional<Status> find_by_id_failure;
    mutable std::optional<Status> find_failure;
    mutable std::optional<Status> overlap_failure;
    int64_t next_id = 900;
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

/**
 * @brief 构造测试日程。
 * @param id 日程标识。
 * @param event 日程标题。
 * @param start_seconds 开始时间 Unix 秒。
 * @param end_seconds 结束时间 Unix 秒，0 表示不设置。
 * @return 完整填充的日程对象。
 */
Schedule StoredSchedule(int64_t id, std::string event, int64_t start_seconds, int64_t end_seconds = 0) {
    Schedule schedule;
    schedule.id = id;
    schedule.event = std::move(event);
    schedule.start_time = DateTime{std::chrono::seconds{start_seconds}};
    if (end_seconds > 0) {
        schedule.end_time = DateTime{std::chrono::seconds{end_seconds}};
    }
    schedule.status = ScheduleStatus::kActive;
    return schedule;
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

    schedules.Reset({StoredSchedule(1, "可取消日程", 1'900'000'000)});
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

    schedules.Reset({StoredSchedule(2, "冲突更新源", 1'900'000'000, 1'900'003'600),
                     StoredSchedule(3, "冲突目标", 1'899'000'000, 1'901'000'000)});
    const auto update_conflict = server.call({
        .request_id = "update-conflict",
        .name = "schedule.update",
        .arguments = {{"schedule_id", int64_t{2}}, {"start_time", std::string("2030-03-17 18:43:20")}},
    });
    Check(OutputString(update_conflict, "status") == "conflict", "一次性日程更新冲突应返回 conflict");

    FailingScheduleRepository failing_schedules;
    ScheduleService failing_service(failing_schedules);
    McpServer failing_server;
    Check(voicelife::mcp::RegisterScheduleMcpTools(failing_server, failing_service).ok(), "失败注入工具应注册成功");

    failing_schedules.insert_failure = Status::Error(ErrorCode::kUnavailable, "插入失败");
    const auto create_insert_failed = failing_server.call({
        .request_id = "create-insert-failed",
        .name = "schedule.create",
        .arguments = {{"event", std::string("插入失败日程")}},
    });
    Check(OutputString(create_insert_failed, "status") == "failure", "插入失败应返回 failure");

    failing_schedules.schedules = {StoredSchedule(20, "待更新日程", 1'900'000'000)};
    failing_schedules.update_failure = Status::Error(ErrorCode::kUnavailable, "更新失败");
    const auto update_store_failed = failing_server.call({
        .request_id = "update-store-failed",
        .name = "schedule.update",
        .arguments = {{"schedule_id", int64_t{20}}, {"event", std::string("更新失败")}},
    });
    Check(OutputString(update_store_failed, "status") == "failure", "更新持久化失败应返回 failure");

    failing_schedules.update_failure.reset();
    failing_schedules.overlap_failure = Status::Error(ErrorCode::kUnavailable, "重叠查询失败");
    const auto update_overlap_failed = failing_server.call({
        .request_id = "update-overlap-failed",
        .name = "schedule.update",
        .arguments = {{"schedule_id", int64_t{20}}, {"start_time", std::string("2030-03-17 18:43:20")}},
    });
    Check(OutputString(update_overlap_failed, "status") == "failure", "更新前重叠查询失败应返回 failure");

    failing_schedules.overlap_failure.reset();
    failing_schedules.find_by_id_failure = Status::Error(ErrorCode::kUnavailable, "读取失败");
    const auto update_load_failed = failing_server.call({
        .request_id = "update-load-failed",
        .name = "schedule.update",
        .arguments = {{"schedule_id", int64_t{20}}, {"notes", std::string("读取失败")}},
    });
    Check(OutputString(update_load_failed, "status") == "failure", "更新前读取失败应返回 failure");

    failing_schedules.find_by_id_failure.reset();
    failing_schedules.delete_failure = Status::Error(ErrorCode::kUnavailable, "取消失败");
    const auto update_cancel_failed = failing_server.call({
        .request_id = "update-cancel-failed",
        .name = "schedule.update",
        .arguments = {{"schedule_id", int64_t{20}},
                      {"expected_event", std::string("待更新日程")},
                      {"expected_start_time", std::string("2030-03-18 01:46:40")},
                      {"status", std::string("cancelled")}},
    });
    Check(OutputString(update_cancel_failed, "status") == "failure", "update 取消失败应返回 failure");

    failing_schedules.delete_failure.reset();
    failing_schedules.find_failure = Status::Error(ErrorCode::kUnavailable, "删除快照失败");
    const auto delete_snapshot_failed = failing_server.call({
        .request_id = "delete-snapshot-failed",
        .name = "schedule.delete",
        .arguments = {{"schedule_id", int64_t{20}}},
    });
    Check(OutputString(delete_snapshot_failed, "status") == "failure", "删除前快照失败应返回 failure");

    failing_schedules.find_failure.reset();
    failing_schedules.delete_failure = Status::Error(ErrorCode::kUnavailable, "删除取消失败");
    const auto delete_cancel_failed = failing_server.call({
        .request_id = "delete-cancel-failed",
        .name = "schedule.delete",
        .arguments = {{"schedule_id", int64_t{20}},
                      {"expected_event", std::string("待更新日程")},
                      {"expected_start_time", std::string("2030-03-18 01:46:40")}},
    });
    Check(OutputString(delete_cancel_failed, "status") == "failure", "删除取消失败应返回 failure");

    return 0;
}
