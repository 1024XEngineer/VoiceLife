#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "im_runtime_test_support.h"
#include "support/in_memory_schedule_repository.h"
#include "support/test_support.h"
#include "voicelife/contracts/im/reminder_action_status_report.h"
#include "voicelife/contracts/json.h"
#include "voicelife/mcp/mcp_server.h"
#include "voicelife/mcp/schedule_mcp_tools.h"
#include "voicelife/schedule/schedule_exception_repository.h"
#include "voicelife/schedule/schedule_operation_service.h"
#include "voicelife/schedule/schedule_reminder_service.h"
#include "voicelife/schedule/schedule_rule_repository.h"
#include "voicelife/schedule/schedule_rule_service.h"
#include "voicelife/schedule/schedule_service.h"
#include "voicelife/storage_memory/memory_schedule_reminder_task_repository.h"
#include "voicelife/timing/timing_task.h"

using voicelife::ErrorCode;
using voicelife::JsonValue;
using voicelife::Result;
using voicelife::Status;
using voicelife::ToolResult;
using voicelife::im::ImTransportStatus;
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
using voicelife::test::im_runtime_support::RuntimeFixture;
using voicelife::timing::CancelTaskCommand;
using voicelife::timing::CancelTaskResult;
using voicelife::timing::CommandAcceptance;
using voicelife::timing::InMemoryTimingTaskRunner;
using voicelife::timing::RegisterTaskCommand;
using voicelife::timing::RegisterTaskResult;
using voicelife::timing::TriggerAt;

namespace {

TriggerAt Trigger(int64_t seconds) { return TriggerAt{std::chrono::seconds{seconds}}; }

DateTime CurrentTime() { return std::chrono::time_point_cast<std::chrono::seconds>(std::chrono::system_clock::now()); }

class FakeSpeech final : public ScheduleReminderSpeechPort {
   public:
    Status SpeakScheduleReminder(std::string_view text) override {
        texts.emplace_back(text);
        return Status::Ok();
    }

    std::vector<std::string> texts;
};

class ScriptedTimingService final : public voicelife::timing::TimingTaskService {
   public:
    CommandAcceptance RegisterTask(RegisterTaskCommand command) override {
        ++register_calls;
        if (report_register_result && command.on_result) command.on_result(register_result);
        register_commands.push_back(std::move(command));
        return register_acceptance;
    }

    CommandAcceptance CancelTask(CancelTaskCommand command) override {
        ++cancel_calls;
        if (report_cancel_result && command.on_result) command.on_result(cancel_result);
        cancel_commands.push_back(std::move(command));
        return cancel_acceptance;
    }

    CommandAcceptance register_acceptance = CommandAcceptance::kAccepted;
    CommandAcceptance cancel_acceptance = CommandAcceptance::kAccepted;
    bool report_register_result = false;
    bool report_cancel_result = false;
    RegisterTaskResult register_result = RegisterTaskResult::kRegistered;
    CancelTaskResult cancel_result = CancelTaskResult::kCancelled;
    int register_calls = 0;
    int cancel_calls = 0;
    std::vector<RegisterTaskCommand> register_commands;
    std::vector<CancelTaskCommand> cancel_commands;
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
        if (fail_update) {
            fail_update = false;
            return next_update_failure;
        }
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
        if (fail_cancel_rule) {
            fail_cancel_rule = false;
            return next_cancel_rule_failure;
        }
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
    bool fail_update = false;
    Status next_update_failure = Status::Ok();
    bool fail_cancel_rule = false;
    Status next_cancel_rule_failure = Status::Ok();

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

std::vector<std::string> OutputStringArray(const ToolResult& result, const std::string& key) {
    std::vector<std::string> output;
    if (!result.output.IsObject()) return output;
    for (const auto& field : *result.output.object) {
        if (field.first != key || !field.second->IsArray()) continue;
        for (const auto& item : *field.second->array)
            if (item->IsString()) output.push_back(item->string);
    }
    return output;
}

std::optional<int64_t> OutputInteger(const ToolResult& result, const std::string& key) {
    if (!result.output.IsObject()) return std::nullopt;
    for (const auto& field : *result.output.object) {
        if (field.first == key && field.second->kind == voicelife::ToolOutputValue::Kind::kInteger) {
            return field.second->integer;
        }
    }
    return std::nullopt;
}

JsonValue DailyRepeat(const std::string& start_date = "2099-01-01") {
    return JsonValue::Object({
        {"freq_type", JsonValue::String("daily")},
        {"start_date", JsonValue::String(start_date)},
        {"start_time", JsonValue::String("09:00:00")},
    });
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
    voicelife::storage_memory::MemoryScheduleReminderTaskRepository reminder_tasks;
    ScheduleReminderService reminder(schedules, reminder_tasks, service, rule_service, timing, speech);
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
    const auto created_tasks = reminder_tasks.FindBySchedule(1);
    Check(created_schedule.ok() && created_tasks.ok() && created_tasks.value->size() == 1,
          "创建未来日程应持久化独立提醒任务");
    const std::string first_task_id = *created_tasks.value->front().timing_task_id;

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
    const auto updated_tasks = reminder_tasks.FindBySchedule(1);
    Check(updated_schedule.ok() && updated_tasks.ok() && updated_tasks.value->size() == 2 &&
              updated_tasks.value->back().timing_task_id.has_value() &&
              *updated_tasks.value->back().timing_task_id != first_task_id,
          "修改后应创建新的独立提醒链");

    const auto old_fire = timing.RunDueTasks(Trigger(1'893'459'600));
    Check(old_fire.processed_count == 0 && speech.texts.empty(), "旧提醒任务取消后不应触发");

    const auto deleted = server.call({
        .request_id = "delete-reminder",
        .name = "schedule.delete",
        .arguments = {{"schedule_id", int64_t{1}},
                      {"expected_event", std::string("第二次提醒")},
                      {"expected_start_time", std::string("2030-01-02 09:00:00")}},
    });
    Check(deleted.status.ok() && OutputString(deleted, "status") == "success", "删除提醒日程应成功");
    const auto deleted_schedule = schedules.FindById(1);
    const auto deleted_tasks = reminder_tasks.FindBySchedule(1);
    Check(deleted_schedule.ok() && deleted_schedule.value->status == ScheduleStatus::kCancelled && deleted_tasks.ok() &&
              !deleted_tasks.value->empty() &&
              deleted_tasks.value->back().timer_status == voicelife::schedule::ScheduleReminderTimerStatus::kCancelled,
          "删除后应取消日程并保留已取消的提醒任务记录");
    Check(timing.RunDueTasks(Trigger(1'893'546'000)).processed_count == 0 && speech.texts.empty(),
          "删除后的新提醒任务也不应触发");
}

struct ReminderToolFixture {
    ReminderToolFixture()
        : rules(schedules, exceptions),
          rule_service(rules, exceptions, schedules),
          service(schedules),
          operation_service(schedules),
          reminder(schedules, reminder_tasks, service, rule_service, timing, speech) {}

    InMemoryScheduleRepository schedules;
    FakeExceptionRepository exceptions;
    FakeRuleRepository rules;
    ScheduleRuleService rule_service;
    ScheduleService service;
    ScheduleOperationService operation_service;
    voicelife::storage_memory::MemoryScheduleReminderTaskRepository reminder_tasks;
    ScriptedTimingService timing;
    FakeSpeech speech;
    ScheduleReminderService reminder;
    McpServer server;
};

void CheckReminderSyncFailurePaths() {
    ReminderToolFixture fixture;
    fixture.timing.register_acceptance = CommandAcceptance::kUnavailable;
    Check(fixture.reminder.Start().ok(), "工具失败测试应能启动空服务");
    Check(voicelife::mcp::RegisterScheduleMcpTools(fixture.server, fixture.service, fixture.rule_service,
                                                   fixture.operation_service, &fixture.reminder)
              .ok(),
          "带提醒服务工具应注册成功");

    const auto create_failed = fixture.server.call({
        .request_id = "create-sync-failed",
        .name = "schedule.create",
        .arguments = {{"event", std::string("创建同步失败")}, {"start_time", std::string("2030-01-01 09:00:00")}},
    });
    Check(create_failed.status.ok() && OutputString(create_failed, "status") == "failure" &&
              OutputString(create_failed, "message").find("提醒同步失败") != std::string::npos,
          "创建保存成功但提醒注册不可用时应返回同步失败");

    fixture.timing.register_acceptance = CommandAcceptance::kAccepted;
    const auto created = fixture.server.call({
        .request_id = "create-then-update",
        .name = "schedule.create",
        .arguments = {{"event", std::string("待修改提醒")}, {"start_time", std::string("2030-01-02 09:00:00")}},
    });
    Check(created.status.ok() && OutputString(created, "status") == "success", "正常创建应成功");
    fixture.timing.cancel_acceptance = CommandAcceptance::kUnavailable;
    const auto update_failed = fixture.server.call({
        .request_id = "update-sync-failed",
        .name = "schedule.update",
        .arguments = {{"schedule_id", int64_t{2}}, {"start_time", std::string("2030-01-03 09:00:00")}},
    });
    Check(update_failed.status.ok() && OutputString(update_failed, "status") == "failure" &&
              OutputString(update_failed, "message").find("提醒同步失败") != std::string::npos,
          "修改保存成功但旧提醒取消失败时应返回同步失败");

    const auto delete_failed = fixture.server.call({
        .request_id = "delete-cancel-failed",
        .name = "schedule.delete",
        .arguments = {{"schedule_id", int64_t{2}},
                      {"expected_event", std::string("待修改提醒")},
                      {"expected_start_time", std::string("2030-01-03 09:00:00")}},
    });
    Check(delete_failed.status.ok() && OutputString(delete_failed, "status") == "failure" &&
              OutputString(delete_failed, "message").find("提醒取消失败") != std::string::npos,
          "删除保存成功但提醒取消失败时应返回取消失败");
}

void CheckOperationServiceOverloadWithoutReminder() {
    InMemoryScheduleRepository schedules;
    FakeExceptionRepository exceptions;
    FakeRuleRepository rules(schedules, exceptions);
    ScheduleRuleService rule_service(rules, exceptions, schedules);
    ScheduleService service(schedules);
    ScheduleOperationService operation_service(schedules);
    McpServer server;
    Check(voicelife::mcp::RegisterScheduleMcpTools(server, service, rule_service, operation_service).ok(),
          "四参数日程工具重载应注册成功");

    const auto created = server.call({
        .request_id = "create-without-reminder",
        .name = "schedule.create",
        .arguments = {{"event", std::string("无提醒服务日程")}, {"start_time", std::string("2030-01-01 09:00:00")}},
    });
    Check(created.status.ok() && OutputString(created, "status") == "success", "未接入提醒服务时创建日程仍应成功");
}

void CheckRuleReminderSuccessPaths() {
    ReminderToolFixture fixture;
    Check(fixture.reminder.Start().ok(), "规则提醒成功路径测试应启动服务");
    Check(voicelife::mcp::RegisterScheduleMcpTools(fixture.server, fixture.service, fixture.rule_service,
                                                   fixture.operation_service, &fixture.reminder)
              .ok(),
          "带提醒服务工具应注册成功");

    const auto created = fixture.server.call({
        .request_id = "create-rule-reminder-success",
        .name = "schedule.create",
        .arguments = {{"event", std::string("可同步规则")}, {"repeat", DailyRepeat("2099-01-01")}},
    });
    Check(created.status.ok() && OutputString(created, "status") == "success", "创建周期规则并同步提醒应成功");
    const ScheduleRuleId rule_id = fixture.rules.rules.back().id;

    const auto updated = fixture.server.call({
        .request_id = "update-rule-reminder-success",
        .name = "schedule.update",
        .arguments = {{"rule_id", int64_t{rule_id}}, {"event", std::string("更新后的可同步规则")}},
    });
    Check(updated.status.ok() && OutputString(updated, "status") == "success", "更新周期规则并同步提醒应成功");

    const auto deleted = fixture.server.call({
        .request_id = "delete-rule-reminder-success",
        .name = "schedule.delete",
        .arguments = {{"rule_id", int64_t{rule_id}}},
    });
    Check(deleted.status.ok() && OutputString(deleted, "status") == "success", "删除周期规则并撤销提醒应成功");
}

void CheckRuleReminderRollbackSyncPaths() {
    ReminderToolFixture update_fixture;
    Check(update_fixture.reminder.Start().ok(), "规则更新失败同步测试应启动服务");
    Check(voicelife::mcp::RegisterScheduleMcpTools(update_fixture.server, update_fixture.service,
                                                   update_fixture.rule_service, update_fixture.operation_service,
                                                   &update_fixture.reminder)
              .ok(),
          "带提醒服务工具应注册成功");
    const auto update_created = update_fixture.server.call({
        .request_id = "create-rule-before-update-failure",
        .name = "schedule.create",
        .arguments = {{"event", std::string("更新失败前规则")}, {"repeat", DailyRepeat("2099-01-01")}},
    });
    Check(update_created.status.ok() && OutputString(update_created, "status") == "success",
          "更新失败同步测试应先创建规则");
    update_fixture.rules.fail_update = true;
    update_fixture.rules.next_update_failure = Status::Error(ErrorCode::kUnavailable, "规则更新失败");
    const auto update_failed = update_fixture.server.call({
        .request_id = "update-rule-then-sync",
        .name = "schedule.update",
        .arguments = {{"rule_id", int64_t{update_fixture.rules.rules.back().id}}, {"event", std::string("更新失败")}},
    });
    Check(update_failed.status.ok() && OutputString(update_failed, "status") == "failure",
          "规则更新失败时应返回失败并保留已撤销提醒的可恢复路径");

    ReminderToolFixture delete_fixture;
    Check(delete_fixture.reminder.Start().ok(), "规则删除失败同步测试应启动服务");
    Check(voicelife::mcp::RegisterScheduleMcpTools(delete_fixture.server, delete_fixture.service,
                                                   delete_fixture.rule_service, delete_fixture.operation_service,
                                                   &delete_fixture.reminder)
              .ok(),
          "带提醒服务工具应注册成功");
    const auto delete_created = delete_fixture.server.call({
        .request_id = "create-rule-before-delete-failure",
        .name = "schedule.create",
        .arguments = {{"event", std::string("删除失败前规则")}, {"repeat", DailyRepeat("2099-01-01")}},
    });
    Check(delete_created.status.ok() && OutputString(delete_created, "status") == "success",
          "删除失败同步测试应先创建规则");
    delete_fixture.rules.fail_cancel_rule = true;
    delete_fixture.rules.next_cancel_rule_failure = Status::Error(ErrorCode::kUnavailable, "规则删除失败");
    const auto delete_failed = delete_fixture.server.call({
        .request_id = "delete-rule-then-sync",
        .name = "schedule.delete",
        .arguments = {{"rule_id", int64_t{delete_fixture.rules.rules.back().id}}},
    });
    Check(delete_failed.status.ok() && OutputString(delete_failed, "status") == "failure",
          "规则删除失败时应返回失败并保留提醒同步恢复路径");
}

void CheckRuleReminderSyncFailurePaths() {
    ReminderToolFixture create_fail_fixture;
    create_fail_fixture.timing.register_acceptance = CommandAcceptance::kUnavailable;
    Check(create_fail_fixture.reminder.Start().ok(), "规则创建同步失败测试应启动服务");
    Check(voicelife::mcp::RegisterScheduleMcpTools(create_fail_fixture.server, create_fail_fixture.service,
                                                   create_fail_fixture.rule_service,
                                                   create_fail_fixture.operation_service, &create_fail_fixture.reminder)
              .ok(),
          "带提醒服务工具应注册成功");

    const auto rule_create_failed = create_fail_fixture.server.call({
        .request_id = "create-rule-sync-failed",
        .name = "schedule.create",
        .arguments = {{"event", std::string("创建规则失败")}, {"repeat", DailyRepeat("2099-01-01")}},
    });
    Check(rule_create_failed.status.ok() && OutputString(rule_create_failed, "status") == "failure" &&
              OutputString(rule_create_failed, "message").find("提醒同步失败") != std::string::npos,
          "周期规则创建后提醒同步不可用时应返回失败");

    ReminderToolFixture fixture;
    Check(fixture.reminder.Start().ok(), "规则撤销失败测试应启动服务");
    Check(voicelife::mcp::RegisterScheduleMcpTools(fixture.server, fixture.service, fixture.rule_service,
                                                   fixture.operation_service, &fixture.reminder)
              .ok(),
          "带提醒服务工具应注册成功");
    const auto rule_create = fixture.server.call({
        .request_id = "create-rule-for-update",
        .name = "schedule.create",
        .arguments = {{"event", std::string("可更新规则")}, {"repeat", DailyRepeat("2099-01-01")}},
    });
    Check(rule_create.status.ok() && OutputString(rule_create, "status") == "success", "正常周期规则创建应成功");

    fixture.timing.cancel_acceptance = CommandAcceptance::kUnavailable;
    const auto rule_update_failed = fixture.server.call({
        .request_id = "update-rule-suspend-failed",
        .name = "schedule.update",
        .arguments = {{"rule_id", int64_t{600}}, {"event", std::string("更新规则失败")}},
    });
    Check(rule_update_failed.status.ok() && OutputString(rule_update_failed, "status") == "failure" &&
              OutputString(rule_update_failed, "message").find("旧提醒撤销失败") != std::string::npos,
          "规则修改前撤销旧提醒不可用时应返回失败");

    const auto rule_delete_failed = fixture.server.call({
        .request_id = "delete-rule-suspend-failed",
        .name = "schedule.delete",
        .arguments = {{"rule_id", int64_t{600}}},
    });
    Check(rule_delete_failed.status.ok() && OutputString(rule_delete_failed, "status") == "failure" &&
              OutputString(rule_delete_failed, "message").find("旧提醒撤销失败") != std::string::npos,
          "规则删除前撤销旧提醒不可用时应返回失败");
}

void CheckReminderActionTools() {
    ReminderToolFixture fixture;
    Check(fixture.reminder.Start().ok(), "提醒动作工具测试应启动服务");
    Check(voicelife::mcp::RegisterScheduleMcpTools(fixture.server, fixture.service, fixture.rule_service,
                                                   fixture.operation_service, &fixture.reminder)
              .ok(),
          "提醒动作工具应注册成功");

    const DateTime now = CurrentTime();
    const auto schedule = fixture.schedules.Insert({
        .event = "动作提醒",
        .start_time = now - std::chrono::minutes{1},
        .created_at = now - std::chrono::minutes{2},
        .updated_at = now - std::chrono::minutes{2},
    });
    Check(schedule.ok(), "应准备动作提醒日程");
    const auto triggered = fixture.reminder_tasks.Insert({
        .schedule_id = schedule.value->id,
        .chain_id = 100,
        .attempt = 1,
        .timing_task_id = "triggered-action-reminder",
        .trigger_at = now - std::chrono::minutes{1},
        .business_status = voicelife::schedule::ScheduleReminderBusinessStatus::kWaitingAcknowledgement,
        .timer_status = voicelife::schedule::ScheduleReminderTimerStatus::kTriggered,
        .triggered_at = now - std::chrono::minutes{1},
        .created_at = now - std::chrono::minutes{2},
        .updated_at = now - std::chrono::minutes{1},
    });
    const auto pending = fixture.reminder_tasks.Insert({
        .schedule_id = schedule.value->id,
        .chain_id = 100,
        .attempt = 2,
        .timing_task_id = "pending-action-reminder",
        .trigger_at = now + std::chrono::minutes{9},
        .created_at = now - std::chrono::minutes{1},
        .updated_at = now - std::chrono::minutes{1},
    });
    Check(triggered.ok() && pending.ok(), "应准备已触发提醒和默认后续提醒");

    const auto snoozed = fixture.server.call({
        .request_id = "snooze-reminder",
        .name = "schedule.reminder_snooze",
        .arguments = {},
    });
    Check(snoozed.status.ok(), "延迟工具调用边界应成功");
    Check(OutputString(snoozed, "status") == "success", "延迟工具业务状态应成功");
    Check(OutputString(snoozed, "message") == "已延迟提醒", "延迟工具应返回固定文案");
    Check(OutputInteger(snoozed, "affected_count") == 1, "延迟工具应影响一条提醒链");
    Check(fixture.timing.register_calls == 0 && fixture.timing.cancel_calls == 0,
          "延迟工具必须复用默认后续提醒且不注册或取消定时器");

    const auto repeated_snooze = fixture.server.call({
        .request_id = "snooze-reminder-again",
        .name = "schedule.reminder_snooze",
        .arguments = {},
    });
    Check(repeated_snooze.status.ok() && OutputString(repeated_snooze, "status") == "failure",
          "已延迟终态不能被重复动作复活");

    // acknowledge 与 snooze 是同一提醒的互斥终态；分别使用独立 fixture 验证两条成功路径。
    ReminderToolFixture acknowledge_fixture;
    Check(acknowledge_fixture.reminder.Start().ok(), "确认动作 fixture 应启动提醒服务");
    Check(voicelife::mcp::RegisterScheduleMcpTools(acknowledge_fixture.server, acknowledge_fixture.service,
                                                   acknowledge_fixture.rule_service,
                                                   acknowledge_fixture.operation_service, &acknowledge_fixture.reminder)
              .ok(),
          "确认动作工具应注册成功");
    const DateTime acknowledge_now = CurrentTime();
    const auto acknowledge_schedule = acknowledge_fixture.schedules.Insert({
        .event = "确认动作提醒",
        .start_time = acknowledge_now - std::chrono::minutes{1},
        .created_at = acknowledge_now - std::chrono::minutes{2},
        .updated_at = acknowledge_now - std::chrono::minutes{2},
    });
    const auto acknowledge_triggered = acknowledge_fixture.reminder_tasks.Insert({
        .schedule_id = acknowledge_schedule.value->id,
        .chain_id = 200,
        .attempt = 1,
        .timing_task_id = "triggered-acknowledge-reminder",
        .trigger_at = acknowledge_now - std::chrono::minutes{1},
        .business_status = voicelife::schedule::ScheduleReminderBusinessStatus::kWaitingAcknowledgement,
        .timer_status = voicelife::schedule::ScheduleReminderTimerStatus::kTriggered,
        .triggered_at = acknowledge_now - std::chrono::minutes{1},
        .created_at = acknowledge_now - std::chrono::minutes{2},
        .updated_at = acknowledge_now - std::chrono::minutes{1},
    });
    const auto acknowledge_pending = acknowledge_fixture.reminder_tasks.Insert({
        .schedule_id = acknowledge_schedule.value->id,
        .chain_id = 200,
        .attempt = 2,
        .timing_task_id = "pending-acknowledge-reminder",
        .trigger_at = acknowledge_now + std::chrono::minutes{9},
        .created_at = acknowledge_now - std::chrono::minutes{1},
        .updated_at = acknowledge_now - std::chrono::minutes{1},
    });
    Check(acknowledge_schedule.ok() && acknowledge_triggered.ok() && acknowledge_pending.ok(),
          "应准备确认动作提醒和后续任务");

    const auto acknowledged = acknowledge_fixture.server.call({
        .request_id = "acknowledge-reminder",
        .name = "schedule.reminder_acknowledge",
        .arguments = {},
    });
    const auto completed_schedule = acknowledge_fixture.schedules.FindById(acknowledge_schedule.value->id);
    const auto tasks = acknowledge_fixture.reminder_tasks.FindBySchedule(acknowledge_schedule.value->id);
    Check(acknowledged.status.ok() && OutputString(acknowledged, "status") == "success", "确认工具必须返回成功状态");
    Check(OutputString(acknowledged, "message") == "已确认提醒", "确认工具必须返回确认文案");
    Check(OutputInteger(acknowledged, "affected_count") == 1, "确认工具必须影响一条提醒链");
    Check(OutputStringArray(acknowledged, "events") == std::vector<std::string>{acknowledge_schedule.value->event},
          "确认工具必须返回日程事件摘要");
    Check(acknowledge_fixture.timing.cancel_calls == 1, "确认工具必须取消后续提醒");
    Check(completed_schedule.ok() && completed_schedule.value->status == ScheduleStatus::kCompleted,
          "确认工具必须完成关联日程");
    Check(tasks.ok() && tasks.value->size() == 2, "确认工具必须保留整条提醒链");
    Check(tasks.value->front().business_status == voicelife::schedule::ScheduleReminderBusinessStatus::kAcknowledged,
          "确认工具必须确认已触发提醒");
    Check(tasks.value->back().timer_status == voicelife::schedule::ScheduleReminderTimerStatus::kCancelled,
          "确认工具必须取消后续任务");

    const auto repeated = acknowledge_fixture.server.call({
        .request_id = "acknowledge-reminder-again",
        .name = "schedule.reminder_acknowledge",
        .arguments = {},
    });
    Check(repeated.status.ok() && OutputString(repeated, "status") == "failure", "已确认终态不能被重复动作复活");
}

void CheckVoiceReminderReporting() {
    ReminderToolFixture fixture;
    RuntimeFixture runtime_fixture;
    Check(runtime_fixture.runtime.Start().ok(), "语音动作上报 IM runtime 应进入探测状态");
    Check(runtime_fixture.runtime.ProbeGateway().status == ImTransportStatus::kHttpError,
          "语音动作上报测试 Gateway 探针应返回受控 404");
    Check(runtime_fixture.runtime.reporting_channel() != nullptr, "语音动作上报通道应创建");
    Check(fixture.reminder.Start().ok(), "语音动作上报提醒服务应启动");
    Check(voicelife::mcp::RegisterScheduleMcpTools(fixture.server, fixture.service, fixture.rule_service,
                                                   fixture.operation_service, &fixture.reminder,
                                                   {.runtime = &runtime_fixture.runtime})
              .ok(),
          "带 IM 上下文的提醒工具应注册成功");

    const DateTime now = CurrentTime();
    const auto schedule = fixture.schedules.Insert({
        .event = "孩子的十分钟后提醒",
        .start_time = now - std::chrono::minutes{1},
        .created_at = now - std::chrono::minutes{2},
        .updated_at = now - std::chrono::minutes{2},
    });
    const auto triggered = fixture.reminder_tasks.Insert({
        .schedule_id = schedule.value->id,
        .chain_id = 300,
        .attempt = 1,
        .timing_task_id = "voice-snooze-trigger",
        .trigger_at = now - std::chrono::minutes{1},
        .business_status = voicelife::schedule::ScheduleReminderBusinessStatus::kWaitingAcknowledgement,
        .timer_status = voicelife::schedule::ScheduleReminderTimerStatus::kTriggered,
        .triggered_at = now - std::chrono::minutes{1},
        .created_at = now - std::chrono::minutes{2},
        .updated_at = now - std::chrono::minutes{1},
    });
    const auto pending = fixture.reminder_tasks.Insert({
        .schedule_id = schedule.value->id,
        .chain_id = 300,
        .attempt = 2,
        .timing_task_id = "voice-snooze-follow-up",
        .trigger_at = now + std::chrono::minutes{9},
        .created_at = now - std::chrono::minutes{1},
        .updated_at = now - std::chrono::minutes{1},
    });
    Check(schedule.ok() && triggered.ok() && pending.ok(), "应准备语音先 snooze 的提醒事实");

    runtime_fixture.transport->next_post_response = {
        .status = ImTransportStatus::kSuccess, .status_code = 202, .body = "{}", .message = {}};
    const auto snoozed = fixture.server.call({
        .request_id = "voice-snooze-report",
        .name = "schedule.reminder_snooze",
        .arguments = {},
    });
    Check(snoozed.status.ok() && OutputString(snoozed, "status") == "success" &&
              OutputString(snoozed, "im_delivery") == "submitted",
          "语音 snooze 应在本地提交后立即上报 IM");
    Check(runtime_fixture.transport->last_request.path == "/v1/devices/device-test/reminder-action-status",
          "语音动作事实必须走设备状态上报路径");
    voicelife::JsonValue body;
    Check(voicelife::ParseJson(runtime_fixture.transport->last_request.body, body).ok(),
          "语音动作事实请求体必须是合法 JSON");
    voicelife::contracts::im::ReminderActionStatusReport report;
    Check(voicelife::contracts::im::ParseReminderActionStatusReport(body, report).ok(),
          "语音动作事实必须通过共享契约解析");
    Check(report.action == "snooze" && report.status == "succeeded" && report.nextTriggerAt.has_value() &&
              report.reminderTriggerId == "voice-snooze-trigger" && report.source == "voice",
          "语音 snooze 上报必须包含动作、终态、下一触发时间和提醒归属");
}

}  // namespace

int main() {
    CheckOneShotReminderLifecycle();
    CheckReminderSyncFailurePaths();
    CheckOperationServiceOverloadWithoutReminder();
    CheckRuleReminderSuccessPaths();
    CheckRuleReminderRollbackSyncPaths();
    CheckRuleReminderSyncFailurePaths();
    CheckReminderActionTools();
    CheckVoiceReminderReporting();
    return 0;
}
