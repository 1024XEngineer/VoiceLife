#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "support/in_memory_schedule_repository.h"
#include "support/test_support.h"
#include "voicelife/mcp/mcp_server.h"
#include "voicelife/mcp/schedule_mcp_tools.h"
#include "voicelife/schedule/schedule_exception_repository.h"
#include "voicelife/schedule/schedule_operation_service.h"
#include "voicelife/schedule/schedule_reminder_service.h"
#include "voicelife/schedule/schedule_rule_repository.h"
#include "voicelife/schedule/schedule_rule_service.h"
#include "voicelife/schedule/schedule_service.h"
#include "voicelife/timing/timing_task.h"

using voicelife::ErrorCode;
using voicelife::Result;
using voicelife::Status;
using voicelife::ToolResult;
using voicelife::mcp::McpServer;
using voicelife::schedule::DateTime;
using voicelife::schedule::Schedule;
using voicelife::schedule::ScheduleException;
using voicelife::schedule::ScheduleOperationService;
using voicelife::schedule::ScheduleReminderService;
using voicelife::schedule::ScheduleReminderSpeechPort;
using voicelife::schedule::ScheduleRule;
using voicelife::schedule::ScheduleRuleId;
using voicelife::schedule::ScheduleRuleService;
using voicelife::schedule::ScheduleService;
using voicelife::schedule::ScheduleStatus;
using voicelife::test::Check;
using voicelife::test::InMemoryScheduleRepository;
using voicelife::timing::InMemoryTimingTaskRunner;
using voicelife::timing::TriggerAt;

namespace {

TriggerAt Trigger(int64_t seconds) { return TriggerAt{std::chrono::seconds{seconds}}; }

class FakeSpeech final : public ScheduleReminderSpeechPort {
   public:
    Status SpeakScheduleReminder(std::string_view text) override {
        texts.emplace_back(text);
        return Status::Ok();
    }

    std::vector<std::string> texts;
};

class FakeExceptionRepository final : public voicelife::schedule::ScheduleExceptionRepository {
   public:
    Result<ScheduleException> Upsert(const ScheduleException& exception) override {
        exceptions.push_back(exception);
        return Result<ScheduleException>::Success(exception);
    }

    Result<std::vector<ScheduleException>> FindByRule(ScheduleRuleId rule_id) const override {
        std::vector<ScheduleException> matched;
        for (const auto& exception : exceptions) {
            if (exception.rule_id == rule_id) matched.push_back(exception);
        }
        return Result<std::vector<ScheduleException>>::Success(std::move(matched));
    }

    Result<std::optional<ScheduleException>> FindByRuleAndTime(ScheduleRuleId rule_id,
                                                               DateTime original_start_time) const override {
        for (const auto& exception : exceptions) {
            if (exception.rule_id == rule_id && exception.original_start_time == original_start_time) {
                return Result<std::optional<ScheduleException>>::Success(exception);
            }
        }
        return Result<std::optional<ScheduleException>>::Success(std::nullopt);
    }

    Status DeleteFuture(ScheduleRuleId rule_id, DateTime after) override {
        (void)rule_id;
        (void)after;
        return Status::Ok();
    }

    std::vector<ScheduleException> exceptions;
};

class FakeRuleRepository final : public voicelife::schedule::ScheduleRuleRepository {
   public:
    FakeRuleRepository(InMemoryScheduleRepository& schedules, FakeExceptionRepository& exceptions)
        : schedules_(schedules), exceptions_(exceptions) {}

    Result<ScheduleRule> Insert(const ScheduleRule& rule) override {
        ScheduleRule stored = rule;
        stored.id = next_rule_id_++;
        rules.push_back(stored);
        return Result<ScheduleRule>::Success(stored);
    }

    Status Update(const ScheduleRule& rule) override {
        for (ScheduleRule& existing : rules) {
            if (existing.id == rule.id) {
                existing = rule;
                return Status::Ok();
            }
        }
        return Status::Error(ErrorCode::kNotFound, "规则不存在");
    }

    Result<std::vector<ScheduleRule>> FindAll() const override {
        return Result<std::vector<ScheduleRule>>::Success(rules);
    }

    Result<ScheduleRule> FindById(ScheduleRuleId id) const override {
        for (const auto& rule : rules) {
            if (rule.id == id) return Result<ScheduleRule>::Success(rule);
        }
        return Result<ScheduleRule>::Failure(ErrorCode::kNotFound, "规则不存在");
    }

    Result<ScheduleRule> CreateWithFirstInstance(const ScheduleRule& rule,
                                                 const std::optional<Schedule>& first_instance) override {
        const auto created = Insert(rule);
        if (!created.ok()) return created;
        if (first_instance.has_value()) {
            Schedule instance = *first_instance;
            instance.rule_id = created.value->id;
            const auto saved = schedules_.Insert(instance);
            if (!saved.ok()) return Result<ScheduleRule>::Failure(saved.status.code, saved.status.message);
        }
        return created;
    }

    Result<ScheduleRule> UpdateAndRebuild(const ScheduleRule& rule,
                                          const std::optional<Schedule>& first_instance) override {
        const Status updated = Update(rule);
        if (!updated.ok()) return Result<ScheduleRule>::Failure(updated.code, updated.message);
        if (first_instance.has_value()) {
            Schedule instance = *first_instance;
            instance.rule_id = rule.id;
            const auto saved = schedules_.Insert(instance);
            if (!saved.ok()) return Result<ScheduleRule>::Failure(saved.status.code, saved.status.message);
        }
        return FindById(rule.id);
    }

    Status CancelRuleAndInstances(ScheduleRuleId id, int64_t& cancelled_instance_count) override {
        const auto loaded = FindById(id);
        if (!loaded.ok()) return loaded.status;
        ScheduleRule cancelled = *loaded.value;
        cancelled.status = ScheduleStatus::kCancelled;
        const Status updated = Update(cancelled);
        if (!updated.ok()) return updated;
        cancelled_instance_count = 0;
        const auto schedules = schedules_.FindAll();
        if (!schedules.ok()) return schedules.status;
        for (Schedule schedule : *schedules.value) {
            if (schedule.rule_id == id && schedule.status == ScheduleStatus::kActive) {
                schedule.status = ScheduleStatus::kCancelled;
                const Status saved = schedules_.Update(schedule);
                if (!saved.ok()) return saved;
                ++cancelled_instance_count;
            }
        }
        return Status::Ok();
    }

    Result<Schedule> CreateNextInstance(const Schedule& schedule,
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
    int64_t next_rule_id_ = 600;

   private:
    InMemoryScheduleRepository& schedules_;
    FakeExceptionRepository& exceptions_;
};

std::string OutputString(const ToolResult& result, const std::string& key) {
    if (!result.output.IsObject()) return {};
    for (const auto& field : *result.output.object) {
        if (field.first == key && field.second->IsString()) return field.second->string;
    }
    return {};
}

void CheckOneShotReminderLifecycle() {
    InMemoryScheduleRepository schedules;
    FakeExceptionRepository exceptions;
    FakeRuleRepository rules(schedules, exceptions);
    ScheduleRuleService rule_service(rules, exceptions, schedules);
    ScheduleService service(schedules);
    ScheduleOperationService operation_service(schedules);
    InMemoryTimingTaskRunner timing;
    FakeSpeech speech;
    ScheduleReminderService reminder(schedules, service, rule_service, timing, speech);
    McpServer server;
    Check(reminder.Start().ok(), "提醒服务应能启动");
    Check(voicelife::mcp::RegisterScheduleMcpTools(server, service, rule_service, operation_service, &reminder).ok(),
          "带提醒服务的日程工具应注册成功");

    const auto created = server.call({
        .request_id = "create-reminder",
        .name = "schedule.create",
        .arguments = {{"event", std::string("第一次提醒")}, {"start_time", std::string("2030-01-01 09:00:00")}},
    });
    Check(created.status.ok() && OutputString(created, "status") == "success", "创建一次性提醒日程应成功");
    timing.ProcessPendingCommands(Trigger(0));
    const auto created_schedule = schedules.FindById(1);
    Check(created_schedule.ok() && created_schedule.value->reminder_task_id.has_value(),
          "创建未来日程应持久化 reminder_task_id");
    const int64_t first_task_id = *created_schedule.value->reminder_task_id;

    const auto updated = server.call({
        .request_id = "update-reminder",
        .name = "schedule.update",
        .arguments = {{"schedule_id", int64_t{1}},
                      {"event", std::string("第二次提醒")},
                      {"start_time", std::string("2030-01-02 09:00:00")}},
    });
    Check(updated.status.ok() && OutputString(updated, "status") == "success", "修改提醒日程应成功");
    timing.ProcessPendingCommands(Trigger(1));
    const auto updated_schedule = schedules.FindById(1);
    Check(updated_schedule.ok() && updated_schedule.value->reminder_task_id.has_value() &&
              *updated_schedule.value->reminder_task_id != first_task_id,
          "修改后应分配新的 reminder_task_id");

    const auto old_fire = timing.RunDueTasks(Trigger(1'893'459'600));
    Check(old_fire.processed_count == 0 && speech.texts.empty(), "旧提醒任务取消后不应触发");

    const auto deleted = server.call({
        .request_id = "delete-reminder",
        .name = "schedule.delete",
        .arguments = {{"schedule_id", int64_t{1}}},
    });
    Check(deleted.status.ok() && OutputString(deleted, "status") == "success", "删除提醒日程应成功");
    const auto deleted_schedule = schedules.FindById(1);
    Check(deleted_schedule.ok() && deleted_schedule.value->status == ScheduleStatus::kCancelled &&
              !deleted_schedule.value->reminder_task_id.has_value(),
          "删除后应取消状态并清空 reminder_task_id");
    Check(timing.RunDueTasks(Trigger(1'893'546'000)).processed_count == 0 && speech.texts.empty(),
          "删除后的新提醒任务也不应触发");
}

}  // namespace

int main() {
    CheckOneShotReminderLifecycle();
    return 0;
}
