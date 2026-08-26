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
using voicelife::ToolCall;
using voicelife::ToolResult;
using voicelife::mcp::McpServer;
using voicelife::schedule::DateTime;
using voicelife::schedule::ExceptionType;
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

/** @brief 测试用的内存例外仓储。 */
class FakeExceptionRepository final : public voicelife::schedule::ScheduleExceptionRepository {
   public:
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

    [[nodiscard]] voicelife::Result<std::vector<ScheduleException>> FindByRule(
        voicelife::schedule::ScheduleRuleId rule_id) const override {
        std::vector<ScheduleException> matched;
        for (const ScheduleException& exception : exceptions) {
            if (exception.rule_id == rule_id) matched.push_back(exception);
        }
        return voicelife::Result<std::vector<ScheduleException>>::Success(std::move(matched));
    }

    [[nodiscard]] voicelife::Result<std::optional<ScheduleException>> FindByRuleAndTime(
        voicelife::schedule::ScheduleRuleId rule_id, DateTime original_start_time) const override {
        for (const ScheduleException& exception : exceptions) {
            if (exception.rule_id == rule_id && exception.original_start_time == original_start_time) {
                return voicelife::Result<std::optional<ScheduleException>>::Success(exception);
            }
        }
        return voicelife::Result<std::optional<ScheduleException>>::Success(std::nullopt);
    }

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
    FakeRuleRepository(InMemoryScheduleRepository& schedules, FakeExceptionRepository& exceptions)
        : schedules_(schedules), exceptions_(exceptions) {}

    voicelife::Result<ScheduleRule> Insert(const ScheduleRule& rule) override {
        ScheduleRule stored = rule;
        stored.id = next_id_++;
        rules.push_back(stored);
        return voicelife::Result<ScheduleRule>::Success(std::move(stored));
    }

    voicelife::Status Update(const ScheduleRule& rule) override {
        for (ScheduleRule& existing : rules) {
            if (existing.id == rule.id) {
                existing = rule;
                return voicelife::Status::Ok();
            }
        }
        return voicelife::Status::Error(ErrorCode::kNotFound, "规则不存在");
    }

    [[nodiscard]] voicelife::Result<std::vector<ScheduleRule>> FindAll() const override {
        return voicelife::Result<std::vector<ScheduleRule>>::Success(rules);
    }

    [[nodiscard]] voicelife::Result<ScheduleRule> FindById(voicelife::schedule::ScheduleRuleId id) const override {
        for (const ScheduleRule& rule : rules) {
            if (rule.id == id) return voicelife::Result<ScheduleRule>::Success(rule);
        }
        return voicelife::Result<ScheduleRule>::Failure(ErrorCode::kNotFound, "规则不存在");
    }

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

/** @brief 构造每日周期 repeat 对象。 @return 完整周期 repeat JSON 对象。 */
JsonValue DailyRepeat() {
    return JsonValue::Object({
        {"freq_type", JsonValue::String("daily")},
        {"start_date", JsonValue::String("2099-01-01")},
        {"start_time", JsonValue::String("09:00:00")},
    });
}

/** @brief 从工具输出对象中读取字符串字段。 @param result 工具结果。 @param key 字段名。 @return 字段值或空。 */
std::string OutputString(const ToolResult& result, const std::string& key) {
    if (!result.output.IsObject()) return {};
    for (const auto& field : *result.output.object) {
        if (field.first == key && field.second->IsString()) return field.second->string;
    }
    return {};
}

std::size_t OutputArraySize(const ToolResult& result, const std::string& key) {
    if (!result.output.IsObject()) return 0;
    for (const auto& field : *result.output.object) {
        if (field.first == key && field.second->IsArray()) return field.second->array->size();
    }
    return 0;
}

}  // namespace

int main() {
    InMemoryScheduleRepository schedules;
    FakeExceptionRepository exceptions;
    FakeRuleRepository rules(schedules, exceptions);
    ScheduleRuleService rule_service(rules, exceptions, schedules);
    ScheduleService service(schedules);
    McpServer server;
    Check(voicelife::mcp::RegisterScheduleMcpTools(server, service, rule_service).ok(), "日程工具应注册成功");

    const auto listed = server.list_tools();
    Check(listed.total == 4, "日程工具应注册四个工具");

    // schedule.create：一次性日程的各个字段与错误路径。
    const auto one_shot = server.call({
        .request_id = "create-once",
        .name = "schedule.create",
        .arguments = {{"event", std::string("评审 Linx")},
                      {"start_time", std::string("2030-03-18 09:30:00")},
                      {"end_time", std::string("2030-03-18 10:30:00")},
                      {"location", std::string("线上")},
                      {"notes", std::string("记得带材料")}},
    });
    Check(one_shot.status.ok() && OutputString(one_shot, "status") == "success", "一次性日程应创建成功");

    const auto bad_start = server.call({
        .request_id = "create-bad-start",
        .name = "schedule.create",
        .arguments = {{"event", std::string("错误")}, {"start_time", std::string("not-a-date")}},
    });
    Check(OutputString(bad_start, "status") == "failure", "无效 start_time 应返回业务失败");

    const auto bad_end = server.call({
        .request_id = "create-bad-end",
        .name = "schedule.create",
        .arguments = {{"event", std::string("错误")}, {"end_time", std::string("2030-03-18 25:00:00")}},
    });
    Check(OutputString(bad_end, "status") == "failure", "无效 end_time 应返回业务失败");

    // schedule.create：周期日程创建成功，返回规则与物化首条实例。
    const auto rule_create = server.call({
        .request_id = "create-rule",
        .name = "schedule.create",
        .arguments = {{"event", std::string("每日站会")}, {"repeat", DailyRepeat()}},
    });
    Check(rule_create.status.ok() && OutputString(rule_create, "status") == "success", "周期日程应创建成功");

    // schedule.create：周期日程缺少 anchor 字段应失败。
    const auto missing_anchor = server.call({
        .request_id = "create-rule-missing-anchor",
        .name = "schedule.create",
        .arguments = {{"event", std::string("缺字段")},
                      {"repeat", JsonValue::Object({{"freq_type", JsonValue::String("daily")}})}},
    });
    Check(!missing_anchor.status.ok(), "周期日程缺少 anchor 应被参数校验拒绝");

    // schedule.create：周期日程 repeat 非法频率应失败。
    const auto bad_repeat = server.call({
        .request_id = "create-rule-bad-freq",
        .name = "schedule.create",
        .arguments = {{"event", std::string("坏频率")},
                      {"repeat", JsonValue::Object({{"freq_type", JsonValue::String("bad")},
                                                    {"start_date", JsonValue::String("2099-01-01")},
                                                    {"start_time", JsonValue::String("09:00:00")}})}},
    });
    Check(OutputString(bad_repeat, "status") == "failure", "非法 repeat.freq_type 应失败");

    // schedule.query：默认 active 状态查询已物化日程。
    const auto queried = server.call({
        .request_id = "query-active",
        .name = "schedule.query",
        .arguments = {{"status", std::string("active")}},
    });
    Check(queried.status.ok() && OutputString(queried, "status") == "success", "查询应返回成功结果");
    Check(queried.text_output.has_value() && queried.text_output->find("条日程") != std::string::npos,
          "语音查询结果必须提供日程数量");
    Check(queried.text_output.has_value() && queried.text_output->find("第 1 条：") != std::string::npos,
          "语音查询结果必须逐条播报完整日程");

    // schedule.query：带日期范围与关键字，触发规则未来 occurrence 与例外展开。
    const auto queried_range = server.call({
        .request_id = "query-range",
        .name = "schedule.query",
        .arguments = {{"status", std::string("all")},
                      {"keyword", std::string("站会")},
                      {"start_date", std::string("2099-01-01")},
                      {"end_date", std::string("2099-01-31")}},
    });
    Check(queried_range.status.ok() && OutputString(queried_range, "status") == "success", "带范围的查询应成功");

    // schedule.query：日期范围不覆盖未来 occurrence 时走 WithinRange 过滤分支。
    const auto queried_narrow = server.call({
        .request_id = "query-narrow",
        .name = "schedule.query",
        .arguments = {{"start_date", std::string("2030-01-01")}, {"end_date", std::string("2030-01-31")}},
    });
    Check(queried_narrow.status.ok(), "窄范围查询应成功");

    // schedule.query：非法日期与逆序日期应失败。
    const auto bad_query_date = server.call({
        .request_id = "query-bad-date",
        .name = "schedule.query",
        .arguments = {{"start_date", std::string("2099-13-01")}},
    });
    Check(OutputString(bad_query_date, "status") == "failure", "非法 start_date 应失败");

    // 日期边界：真实日历日期与完整字符串都必须通过边界解析器校验，不能把 sscanf 的尾随内容带入领域层。
    const auto impossible_query_date = server.call({
        .request_id = "query-impossible-date",
        .name = "schedule.query",
        .arguments = {{"start_date", std::string("2099-02-29")}},
    });
    Check(OutputString(impossible_query_date, "status") == "failure", "不存在的日历日期应失败");

    const auto trailing_query_date = server.call({
        .request_id = "query-trailing-date",
        .name = "schedule.query",
        .arguments = {{"start_date", std::string("2099-01-01垃圾")}},
    });
    Check(OutputString(trailing_query_date, "status") == "failure", "带尾随文本的日期应失败");

    const auto trailing_create_time = server.call({
        .request_id = "create-trailing-time",
        .name = "schedule.create",
        .arguments = {{"event", std::string("非法时间")}, {"start_time", std::string("2099-01-01 09:00:00extra")}},
    });
    Check(OutputString(trailing_create_time, "status") == "failure", "带尾随文本的时间应失败");

    const auto impossible_repeat_date = server.call({
        .request_id = "create-impossible-repeat-date",
        .name = "schedule.create",
        .arguments = {{"event", std::string("非法周期日期")},
                      {"repeat", JsonValue::Object({{"freq_type", JsonValue::String("daily")},
                                                    {"start_date", JsonValue::String("2099-02-29")},
                                                    {"start_time", JsonValue::String("09:00:00")}})}},
    });
    Check(OutputString(impossible_repeat_date, "status") == "failure", "不存在的周期起始日期应失败");

    const auto unsupported_repeat_field = server.call({
        .request_id = "create-unsupported-repeat-field",
        .name = "schedule.create",
        .arguments = {{"event", std::string("不支持的相对周期")},
                      {"repeat", JsonValue::Object({{"freq_type", JsonValue::String("monthly")},
                                                    {"start_date", JsonValue::String("2099-01-01")},
                                                    {"start_time", JsonValue::String("09:00:00")},
                                                    {"monthly_mode", JsonValue::String("ordinal_weekday")},
                                                    {"weekday_ordinal", JsonValue::Number(2)}})}},
    });
    Check(!unsupported_repeat_field.status.ok() || OutputString(unsupported_repeat_field, "status") == "failure",
          "未声明的周期字段不得被静默忽略");

    const auto reversed_date = server.call({
        .request_id = "query-reversed",
        .name = "schedule.query",
        .arguments = {{"start_date", std::string("2099-02-01")}, {"end_date", std::string("2099-01-01")}},
    });
    Check(OutputString(reversed_date, "status") == "failure", "start_date 晚于 end_date 应失败");

    // schedule.query：覆盖全部状态筛选枚举与非法状态回退。
    for (const char* status : {"all", "active", "cancelled", "completed", "unknown"}) {
        const auto q = server.call({
            .request_id = "query-status",
            .name = "schedule.query",
            .arguments = {{"status", std::string(status)}},
        });
        Check(q.status.ok(), "状态查询应成功");
    }

    // schedule.update：schedule_id 与 rule_id 互斥。
    const auto both_ids = server.call({
        .request_id = "update-both",
        .name = "schedule.update",
        .arguments = {{"schedule_id", int64_t{1}}, {"rule_id", int64_t{600}}},
    });
    Check(OutputString(both_ids, "status") == "failure", "schedule_id 与 rule_id 同用应失败");

    // schedule.update：original_start_time 必须与 rule_id 一起使用。
    const auto orphan_time = server.call({
        .request_id = "update-orphan-time",
        .name = "schedule.update",
        .arguments = {{"original_start_time", std::string("2099-01-05 09:00:00")}},
    });
    Check(OutputString(orphan_time, "status") == "failure", "单独 original_start_time 应失败");

    // schedule.update：按 schedule_id 修改已物化一次性日程。
    const auto update_schedule = server.call({
        .request_id = "update-schedule",
        .name = "schedule.update",
        .arguments = {{"schedule_id", int64_t{1}}, {"event", std::string("评审 Linx（改）")}},
    });
    Check(update_schedule.status.ok() && OutputString(update_schedule, "status") == "success",
          "按 schedule_id 更新应成功");

    // schedule.update：按 schedule_id 修改时非法开始时间应失败。
    const auto update_bad_start = server.call({
        .request_id = "update-bad-start",
        .name = "schedule.update",
        .arguments = {{"schedule_id", int64_t{1}}, {"start_time", std::string("bad")}},
    });
    Check(OutputString(update_bad_start, "status") == "failure", "更新非法开始时间应失败");

    // schedule.update：按 rule_id + original_start_time 跳过未来单次。
    const auto skip_occurrence = server.call({
        .request_id = "update-skip",
        .name = "schedule.update",
        .arguments = {{"rule_id", int64_t{600}},
                      {"original_start_time", std::string("2099-01-05 09:00:00")},
                      {"status", std::string("cancelled")}},
    });
    Check(skip_occurrence.status.ok() && OutputString(skip_occurrence, "status") == "success", "跳过未来单次应成功");

    // schedule.update：按 rule_id + original_start_time 修改未来单次。
    const auto update_occurrence = server.call({
        .request_id = "update-occurrence",
        .name = "schedule.update",
        .arguments = {{"rule_id", int64_t{600}},
                      {"original_start_time", std::string("2099-01-06 09:00:00")},
                      {"event", std::string("改期站会")}},
    });
    Check(update_occurrence.status.ok() && OutputString(update_occurrence, "status") == "success",
          "修改未来单次应成功");

    // schedule.update：按 rule_id 更新整条规则。
    const auto update_rule = server.call({
        .request_id = "update-rule",
        .name = "schedule.update",
        .arguments = {{"rule_id", int64_t{600}}, {"event", std::string("每日站会（改）")}},
    });
    Check(update_rule.status.ok() && OutputString(update_rule, "status") == "success", "更新整条规则应成功");

    // schedule.update：按 rule_id 更新时非法 repeat 应失败。
    const auto update_rule_bad_repeat = server.call({
        .request_id = "update-rule-bad-repeat",
        .name = "schedule.update",
        .arguments = {{"rule_id", int64_t{600}},
                      {"repeat", JsonValue::Object({{"freq_type", JsonValue::String("bad")},
                                                    {"start_date", JsonValue::String("2099-01-01")},
                                                    {"start_time", JsonValue::String("09:00:00")}})}},
    });
    Check(OutputString(update_rule_bad_repeat, "status") == "failure", "非法 repeat 应失败");

    // schedule.update：缺少定位参数应失败。
    const auto update_no_id = server.call({
        .request_id = "update-no-id",
        .name = "schedule.update",
        .arguments = {{"event", std::string("无目标")}},
    });
    Check(OutputString(update_no_id, "status") == "failure", "缺少定位参数应失败");

    // schedule.delete：缺少定位参数应失败。
    const auto delete_no_id = server.call({
        .request_id = "delete-no-id",
        .name = "schedule.delete",
        .arguments = {},
    });
    Check(OutputString(delete_no_id, "status") == "failure", "缺少 schedule_id 或 rule_id 应失败");

    // schedule.delete：schedule_id 与 rule_id 同用应失败。
    const auto delete_both = server.call({
        .request_id = "delete-both",
        .name = "schedule.delete",
        .arguments = {{"schedule_id", int64_t{1}}, {"rule_id", int64_t{600}}},
    });
    Check(OutputString(delete_both, "status") == "failure", "删除时 schedule_id 与 rule_id 同用应失败");

    // schedule.delete：删除不存在的日程应失败。
    const auto delete_missing = server.call({
        .request_id = "delete-missing",
        .name = "schedule.delete",
        .arguments = {{"schedule_id", int64_t{9999}}},
    });
    Check(OutputString(delete_missing, "status") == "failure", "删除不存在日程应失败");

    // schedule.delete：按 schedule_id 删除一次性日程。
    const auto delete_schedule = server.call({
        .request_id = "delete-schedule",
        .name = "schedule.delete",
        .arguments = {{"schedule_id", int64_t{2}},
                      {"expected_event", std::string("每日站会")},
                      {"expected_start_time", std::string("2099-01-01 09:00:00")}},
    });
    Check(delete_schedule.status.ok() && OutputString(delete_schedule, "status") == "success",
          "按 schedule_id 删除应成功");

    // schedule.delete：按 rule_id + original_start_time 删除未来单次。
    const auto delete_occurrence = server.call({
        .request_id = "delete-occurrence",
        .name = "schedule.delete",
        .arguments = {{"rule_id", int64_t{600}}, {"original_start_time", std::string("2099-01-07 09:00:00")}},
    });
    Check(delete_occurrence.status.ok() && OutputString(delete_occurrence, "status") == "success",
          "删除未来单次应成功");

    // schedule.delete：按 rule_id 取消整条规则。
    const auto delete_rule = server.call({
        .request_id = "delete-rule",
        .name = "schedule.delete",
        .arguments = {{"rule_id", int64_t{600}}},
    });
    Check(delete_rule.status.ok() && OutputString(delete_rule, "status") == "success", "取消整条规则应成功");

    // === 补充分支覆盖：冲突、取消、非法字段、例外展开 ===

    // 一次性日程与已有日程冲突（覆盖 ConflictOutput 与一次性冲突分支）。
    const auto conflict_one_shot = server.call({
        .request_id = "conflict-once",
        .name = "schedule.create",
        .arguments = {{"event", std::string("冲突日程")}, {"start_time", std::string("2030-03-18 09:45:00")}},
    });
    Check(OutputString(conflict_one_shot, "status") == "conflict", "一次性日程冲突应返回 conflict");

    // 空事件名触发一次性创建失败（非冲突分支）。
    const auto empty_event = server.call({
        .request_id = "create-empty-event",
        .name = "schedule.create",
        .arguments = {{"event", std::string("")}},
    });
    Check(OutputString(empty_event, "status") == "failure", "空事件名应失败");

    // 周期规则首条实例与已有日程冲突。
    const auto conflict_rule = server.call({
        .request_id = "conflict-rule",
        .name = "schedule.create",
        .arguments = {{"event", std::string("冲突规则")},
                      {"repeat", JsonValue::Object({{"freq_type", JsonValue::String("daily")},
                                                    {"start_date", JsonValue::String("2030-03-18")},
                                                    {"start_time", JsonValue::String("09:30:00")},
                                                    {"end_time", JsonValue::String("10:30:00")}})}},
    });
    Check(OutputString(conflict_rule, "status") == "conflict", "周期规则冲突应返回 conflict");

    // 查询非法 end_date。
    const auto bad_query_end = server.call({
        .request_id = "query-bad-end",
        .name = "schedule.query",
        .arguments = {{"end_date", std::string("2099-13-01")}},
    });
    Check(OutputString(bad_query_end, "status") == "failure", "非法 end_date 应失败");

    // 更新分支：完整字段更新覆盖 start/end/location/notes 赋值。
    const auto update_target = server.call({
        .request_id = "update-target",
        .name = "schedule.create",
        .arguments = {{"event", std::string("更新目标")},
                      {"start_time", std::string("2030-05-01 09:00:00")},
                      {"end_time", std::string("2030-05-01 10:00:00")}},
    });
    Check(update_target.status.ok() && OutputString(update_target, "status") == "success", "更新目标日程应创建成功");

    const auto update_all = server.call({
        .request_id = "update-all",
        .name = "schedule.update",
        .arguments = {{"schedule_id", int64_t{4}},
                      {"start_time", std::string("2030-05-02 09:00:00")},
                      {"end_time", std::string("2030-05-02 10:00:00")},
                      {"location", std::string("新地点")},
                      {"notes", std::string("新备注")}},
    });
    Check(OutputString(update_all, "status") == "success", "更新全部字段应成功");

    const auto update_bad_end = server.call({
        .request_id = "update-bad-end",
        .name = "schedule.update",
        .arguments = {{"schedule_id", int64_t{4}}, {"end_time", std::string("bad")}},
    });
    Check(OutputString(update_bad_end, "status") == "failure", "更新非法 end_time 应失败");

    // 按 schedule_id 取消已物化日程。
    const auto cancel_by_id = server.call({
        .request_id = "update-cancel",
        .name = "schedule.update",
        .arguments = {{"schedule_id", int64_t{4}},
                      {"expected_event", std::string("更新目标")},
                      {"expected_start_time", std::string("2030-05-02 09:00:00")},
                      {"status", std::string("cancelled")}},
    });
    Check(OutputString(cancel_by_id, "status") == "success", "按 schedule_id 取消应成功");

    // 更新不存在的日程。
    const auto update_missing = server.call({
        .request_id = "update-missing",
        .name = "schedule.update",
        .arguments = {{"schedule_id", int64_t{9999}}, {"event", std::string("不存在")}},
    });
    Check(OutputString(update_missing, "status") == "failure", "更新不存在日程应失败");

    // 更新引发时间冲突。
    const auto conflict_target = server.call({
        .request_id = "conflict-target",
        .name = "schedule.create",
        .arguments = {{"event", std::string("冲突目标")}, {"start_time", std::string("2030-06-01 09:00:00")}},
    });
    Check(conflict_target.status.ok(), "冲突目标日程应创建成功");

    const auto update_conflict = server.call({
        .request_id = "update-conflict",
        .name = "schedule.update",
        .arguments = {{"schedule_id", int64_t{5}}, {"start_time", std::string("2030-03-18 09:45:00")}},
    });
    Check(OutputString(update_conflict, "status") == "conflict", "更新冲突应返回 conflict");

    // 未来单次：非法 original_start_time。
    const auto occ_bad_original = server.call({
        .request_id = "occ-bad-original",
        .name = "schedule.update",
        .arguments = {{"rule_id", int64_t{600}}, {"original_start_time", std::string("bad")}},
    });
    Check(OutputString(occ_bad_original, "status") == "failure", "非法 original_start_time 应失败");

    // 未来单次：非法 start_time / end_time。
    const auto occ_bad_start = server.call({
        .request_id = "occ-bad-start",
        .name = "schedule.update",
        .arguments = {{"rule_id", int64_t{600}},
                      {"original_start_time", std::string("2099-01-06 09:00:00")},
                      {"start_time", std::string("bad")}},
    });
    Check(OutputString(occ_bad_start, "status") == "failure", "未来单次非法 start_time 应失败");

    const auto occ_bad_end = server.call({
        .request_id = "occ-bad-end",
        .name = "schedule.update",
        .arguments = {{"rule_id", int64_t{600}},
                      {"original_start_time", std::string("2099-01-06 09:00:00")},
                      {"end_time", std::string("bad")}},
    });
    Check(OutputString(occ_bad_end, "status") == "failure", "未来单次非法 end_time 应失败");

    // 未来单次：合法全字段修改。
    const auto occ_valid = server.call({
        .request_id = "occ-valid",
        .name = "schedule.update",
        .arguments = {{"rule_id", int64_t{600}},
                      {"original_start_time", std::string("2099-01-08 09:00:00")},
                      {"start_time", std::string("2099-01-08 10:00:00")},
                      {"end_time", std::string("2099-01-08 11:00:00")},
                      {"location", std::string("改地点")},
                      {"notes", std::string("改备注")}},
    });
    Check(occ_valid.status.ok() && OutputString(occ_valid, "status") == "success", "未来单次全字段修改应成功");

    // 更新不存在的规则。
    const auto update_rule_missing = server.call({
        .request_id = "update-rule-missing",
        .name = "schedule.update",
        .arguments = {{"rule_id", int64_t{9999}}, {"event", std::string("不存在规则")}},
    });
    Check(OutputString(update_rule_missing, "status") == "failure", "更新不存在规则应失败");

    // 新建活跃规则后更新为冲突时间。
    const auto new_rule = server.call({
        .request_id = "new-rule",
        .name = "schedule.create",
        .arguments = {{"event", std::string("新规则")}, {"repeat", DailyRepeat()}},
    });
    Check(new_rule.status.ok() && OutputString(new_rule, "status") == "success", "新规则应创建成功");

    const auto update_rule_conflict = server.call({
        .request_id = "update-rule-conflict",
        .name = "schedule.update",
        .arguments = {{"rule_id", int64_t{601}},
                      {"repeat", JsonValue::Object({{"freq_type", JsonValue::String("daily")},
                                                    {"start_date", JsonValue::String("2030-03-18")},
                                                    {"start_time", JsonValue::String("09:30:00")},
                                                    {"end_time", JsonValue::String("10:30:00")}})}},
    });
    Check(OutputString(update_rule_conflict, "status") == "conflict", "更新规则冲突应返回 conflict");

    // 删除未来单次：非法 original_start_time。
    const auto delete_bad_original = server.call({
        .request_id = "delete-bad-original",
        .name = "schedule.delete",
        .arguments = {{"rule_id", int64_t{600}}, {"original_start_time", std::string("bad")}},
    });
    Check(OutputString(delete_bad_original, "status") == "failure", "删除未来单次非法时间应失败");

    // 查询范围内包含周期例外时展开返回。
    const auto query_with_exception = server.call({
        .request_id = "query-exception",
        .name = "schedule.query",
        .arguments = {{"status", std::string("all")},
                      {"start_date", std::string("2099-01-01")},
                      {"end_date", std::string("2099-01-31")}},
    });
    Check(query_with_exception.status.ok() && OutputString(query_with_exception, "status") == "success",
          "例外展开查询应成功");

    // === 覆盖周期输出层：weekly/monthly/yearly 频率、月模式、完成态与带结束时间的未来实例 ===

    // weekly 规则：覆盖 FrequencyName 的 weekly 分支，并带 end_time 用于后续未来实例展开。
    const auto weekly_rule = server.call({
        .request_id = "create-weekly",
        .name = "schedule.create",
        .arguments = {{"event", std::string("每周复盘")},
                      {"repeat", JsonValue::Object({{"freq_type", JsonValue::String("weekly")},
                                                    {"start_date", JsonValue::String("2099-01-01")},
                                                    {"start_time", JsonValue::String("08:00:00")},
                                                    {"end_time", JsonValue::String("09:00:00")},
                                                    {"weekdays_mask", JsonValue::Number(1)}})}},
    });
    Check(weekly_rule.status.ok() && OutputString(weekly_rule, "status") == "success", "每周规则应创建成功");

    // monthly last_day 规则：覆盖 FrequencyName monthly 与 MonthlyModeName last_day 分支。
    const auto monthly_last = server.call({
        .request_id = "create-monthly-last",
        .name = "schedule.create",
        .arguments = {{"event", std::string("月末总结")},
                      {"repeat", JsonValue::Object({{"freq_type", JsonValue::String("monthly")},
                                                    {"start_date", JsonValue::String("2099-01-01")},
                                                    {"start_time", JsonValue::String("07:00:00")},
                                                    {"monthly_mode", JsonValue::String("last_day")}})}},
    });
    Check(monthly_last.status.ok() && OutputString(monthly_last, "status") == "success", "月末规则应创建成功");

    // monthly specific_day 规则：覆盖 MonthlyModeName specific_day 分支。
    const auto monthly_day = server.call({
        .request_id = "create-monthly-day",
        .name = "schedule.create",
        .arguments = {{"event", std::string("每月十五号")},
                      {"repeat", JsonValue::Object({{"freq_type", JsonValue::String("monthly")},
                                                    {"start_date", JsonValue::String("2099-01-01")},
                                                    {"start_time", JsonValue::String("06:00:00")},
                                                    {"monthly_mode", JsonValue::String("specific_day")},
                                                    {"day_of_month", JsonValue::Number(15)}})}},
    });
    Check(monthly_day.status.ok() && OutputString(monthly_day, "status") == "success", "指定日期规则应创建成功");

    // yearly 规则：覆盖 FrequencyName yearly 分支。
    const auto yearly_rule = server.call({
        .request_id = "create-yearly",
        .name = "schedule.create",
        .arguments = {{"event", std::string("年度纪念")},
                      {"repeat", JsonValue::Object({{"freq_type", JsonValue::String("yearly")},
                                                    {"start_date", JsonValue::String("2099-01-01")},
                                                    {"start_time", JsonValue::String("05:00:00")},
                                                    {"month_of_year", JsonValue::Number(6)},
                                                    {"day_of_month", JsonValue::Number(15)}})}},
    });
    Check(yearly_rule.status.ok() && OutputString(yearly_rule, "status") == "success", "每年规则应创建成功");

    // 完成态日程：直接写入内存仓储后查询，覆盖 StatusName 的 completed 分支。
    Schedule completed;
    completed.event = "已完成日程";
    completed.status = ScheduleStatus::kCompleted;
    completed.start_time = DateTime{std::chrono::seconds{4'071'171'600}};
    const auto completed_inserted = schedules.Insert(completed);
    Check(completed_inserted.ok(), "完成态日程应插入成功");
    const auto query_completed = server.call({
        .request_id = "query-completed",
        .name = "schedule.query",
        .arguments = {{"status", std::string("completed")}},
    });
    Check(query_completed.status.ok() && OutputString(query_completed, "status") == "success", "完成态查询应成功");

    // 带结束时间的周期规则：查询时展开未来实例，覆盖 FutureOccurrenceOutput 的 end_time 分支。
    const auto query_recurring = server.call({
        .request_id = "query-recurring",
        .name = "schedule.query",
        .arguments = {{"status", std::string("all")},
                      {"start_date", std::string("2099-01-01")},
                      {"end_date", std::string("2099-01-31")}},
    });
    Check(query_recurring.status.ok() && OutputString(query_recurring, "status") == "success",
          "周期未来实例展开查询应成功");

    // 更新整条规则时传入 location 与 notes，覆盖 UpdateRuleCommand 的可选字段赋值分支。
    const auto update_rule_full = server.call({
        .request_id = "update-rule-full",
        .name = "schedule.update",
        .arguments = {{"rule_id", int64_t{601}},
                      {"location", std::string("新会议室")},
                      {"notes", std::string("新备注")}},
    });
    Check(update_rule_full.status.ok() && OutputString(update_rule_full, "status") == "success",
          "带位置与备注更新规则应成功");

    // IM Gateway 对完整查询结果的每个数组最多接受 100 项；查询输出必须在设备端完成截断。
    InMemoryScheduleRepository capped_schedules;
    FakeExceptionRepository capped_exceptions;
    FakeRuleRepository capped_rules(capped_schedules, capped_exceptions);
    for (int64_t index = 0; index < 34; ++index) {
        ScheduleRule rule;
        rule.id = 1'000 + index;
        rule.event = "批量周期日程";
        rule.freq_type = voicelife::schedule::Frequency::kDaily;
        rule.interval_val = 1;
        rule.start_date = {.year = 2030, .month = 1, .day = 1};
        rule.start_time = {.hour = 9, .minute = 0, .second = 0};
        rule.status = ScheduleStatus::kActive;
        capped_rules.rules.push_back(rule);
    }
    for (int64_t index = 0; index < 101; ++index) {
        ScheduleException exception;
        exception.id = 2'000 + index;
        exception.rule_id = 1'000;
        exception.original_start_time = DateTime{std::chrono::seconds{1'893'456'000 + index}};
        exception.type = ExceptionType::kSkip;
        capped_exceptions.exceptions.push_back(std::move(exception));
    }
    ScheduleRuleService capped_rule_service(capped_rules, capped_exceptions, capped_schedules);
    ScheduleService capped_service(capped_schedules);
    McpServer capped_server;
    Check(voicelife::mcp::RegisterScheduleMcpTools(capped_server, capped_service, capped_rule_service).ok(),
          "受限查询日程工具应注册成功");
    const auto capped_query = capped_server.call({
        .request_id = "query-capped-results",
        .name = "schedule.query",
        .arguments = {{"status", std::string("active")}},
    });
    Check(capped_query.status.ok() && OutputString(capped_query, "status") == "success", "大结果查询应成功");
    Check(OutputArraySize(capped_query, "future_occurrences") == 100 &&
              OutputArraySize(capped_query, "exceptions") == 100,
          "查询输出必须限制到 Gateway 每个数组的 100 项上限");

    // 未启用周期日程能力时（2 参数重载），repeat / rule_id 路径应返回明确失败。
    McpServer one_shot_server;
    ScheduleService one_shot_service(schedules);
    Check(voicelife::mcp::RegisterScheduleMcpTools(one_shot_server, one_shot_service).ok(), "2 参数重载应注册成功");

    const auto disabled_rule = one_shot_server.call({
        .request_id = "disabled-rule",
        .name = "schedule.create",
        .arguments = {{"event", std::string("无规则能力")}, {"repeat", DailyRepeat()}},
    });
    Check(OutputString(disabled_rule, "status") == "failure", "未启用周期能力时创建周期日程应失败");

    const auto disabled_update = one_shot_server.call({
        .request_id = "disabled-update",
        .name = "schedule.update",
        .arguments = {{"rule_id", int64_t{600}}},
    });
    Check(OutputString(disabled_update, "status") == "failure", "未启用周期能力时按 rule_id 更新应失败");

    const auto disabled_delete = one_shot_server.call({
        .request_id = "disabled-delete",
        .name = "schedule.delete",
        .arguments = {{"rule_id", int64_t{600}}},
    });
    Check(OutputString(disabled_delete, "status") == "failure", "未启用周期能力时按 rule_id 删除应失败");

    return 0;
}
