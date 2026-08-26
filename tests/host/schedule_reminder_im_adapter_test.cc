#include "schedule_reminder_im_adapter.h"

#include <chrono>
#include <cstdlib>
#include <deque>
#include <string>

#include "support/in_memory_schedule_repository.h"
#include "support/test_support.h"
#include "voicelife/im/im_transport.h"
#include "voicelife/schedule/schedule_exception_repository.h"
#include "voicelife/schedule/schedule_rule_repository.h"
#include "voicelife/storage_memory/memory_schedule_reminder_task_repository.h"
#include "voicelife/timing/timing_task.h"

using voicelife::Result;
using voicelife::Status;
using voicelife::contracts::im::ReminderActionCommand;
using voicelife::im::ImCredentialProvider;
using voicelife::im::ImHttpRequest;
using voicelife::im::ImHttpResponse;
using voicelife::im::ImRuntime;
using voicelife::im::ImRuntimeConfig;
using voicelife::im::ImRuntimeReadinessPort;
using voicelife::im::ImTransport;
using voicelife::im::ImTransportStatus;
using voicelife::runtime::ImScheduleReminderActionExecutor;
using voicelife::runtime::ImScheduleReminderNotification;
using voicelife::schedule::DateTime;
using voicelife::schedule::Schedule;
using voicelife::schedule::ScheduleException;
using voicelife::schedule::ScheduleReminderBusinessStatus;
using voicelife::schedule::ScheduleReminderService;
using voicelife::schedule::ScheduleReminderSpeechPort;
using voicelife::schedule::ScheduleReminderTask;
using voicelife::schedule::ScheduleReminderTimerStatus;
using voicelife::schedule::ScheduleRule;
using voicelife::schedule::ScheduleRuleId;
using voicelife::schedule::ScheduleRuleService;
using voicelife::schedule::ScheduleService;
using voicelife::schedule::ScheduleStatus;
using voicelife::test::Check;
using voicelife::test::InMemoryScheduleRepository;

namespace {

DateTime At(int64_t seconds) { return DateTime{std::chrono::seconds{seconds}}; }

class FakeTransport final : public ImTransport {
   public:
    ImHttpResponse Post(const ImHttpRequest& request) override {
        requests.push_back(request);
        return {.status = ImTransportStatus::kSuccess, .status_code = 202, .body = response_body};
    }
    ImHttpResponse Get(const ImHttpRequest& request) override {
        requests.push_back(request);
        return {.status = ImTransportStatus::kSuccess, .status_code = 200};
    }
    std::string response_body;
    std::vector<ImHttpRequest> requests;
};

class RuntimeInputs final : public voicelife::im::ImConfigProvider,
                            public ImCredentialProvider,
                            public ImRuntimeReadinessPort {
   public:
    Result<ImRuntimeConfig> Load() override {
        return Result<ImRuntimeConfig>::Success(
            {.enabled = true, .gateway_origin = "https://im.example.com", .user_id = "user-1"});
    }
    std::string DeviceToken() const override { return "token-1"; }
    std::string DeviceId() const override { return "device-1"; }
    bool NetworkReady() const override { return true; }
    bool SystemTimeReady() const override { return true; }
};

class Speech final : public ScheduleReminderSpeechPort {
   public:
    Status SpeakScheduleReminder(std::string_view) override { return Status::Ok(); }
};

class Exceptions final : public voicelife::schedule::ScheduleExceptionRepository {
   public:
    Result<ScheduleException> Upsert(const ScheduleException& value) override {
        return Result<ScheduleException>::Success(value);
    }
    Result<std::vector<ScheduleException>> FindByRule(ScheduleRuleId) const override {
        return Result<std::vector<ScheduleException>>::Success({});
    }
    Result<std::optional<ScheduleException>> FindByRuleAndTime(ScheduleRuleId, DateTime) const override {
        return Result<std::optional<ScheduleException>>::Success(std::nullopt);
    }
    Status DeleteFuture(ScheduleRuleId, DateTime) override { return Status::Ok(); }
};

class Rules final : public voicelife::schedule::ScheduleRuleRepository {
   public:
    Result<ScheduleRule> Insert(const ScheduleRule&) override { std::abort(); }
    Status Update(const ScheduleRule&) override { std::abort(); }
    Result<std::vector<ScheduleRule>> FindAll() const override {
        return Result<std::vector<ScheduleRule>>::Success({});
    }
    Result<ScheduleRule> FindById(ScheduleRuleId) const override { std::abort(); }
    Result<ScheduleRule> CreateWithFirstInstance(const ScheduleRule&, const std::optional<Schedule>&) override {
        std::abort();
    }
    Result<ScheduleRule> UpdateAndRebuild(const ScheduleRule&, const std::optional<Schedule>&) override {
        std::abort();
    }
    Status CancelRuleAndInstances(ScheduleRuleId, int64_t&) override { std::abort(); }
    Result<Schedule> CreateNextInstance(const Schedule&, const std::optional<ScheduleException>&) override {
        std::abort();
    }
};

class Timing final : public voicelife::timing::TimingTaskService {
   public:
    voicelife::timing::CommandAcceptance RegisterTask(voicelife::timing::RegisterTaskCommand) override {
        return voicelife::timing::CommandAcceptance::kAccepted;
    }
    voicelife::timing::CommandAcceptance CancelTask(voicelife::timing::CancelTaskCommand) override {
        ++cancel_count;
        return voicelife::timing::CommandAcceptance::kAccepted;
    }
    int cancel_count = 0;
};

}  // namespace

int main() {
    RuntimeInputs inputs;
    FakeTransport* transport_ptr = nullptr;
    ImRuntime runtime(inputs, inputs, inputs, [&transport_ptr](const std::string&) {
        auto created = std::make_unique<FakeTransport>();
        transport_ptr = created.get();
        return created;
    });
    Check(runtime.Start().ok(), "IM Runtime 应进入探针状态");
    Check(runtime.ProbeGateway().status == ImTransportStatus::kSuccess, "IM Runtime 探针应成功");

    std::optional<voicelife::im::ActionWindow> action_window;
    ImScheduleReminderNotification notification(runtime, [&](auto value) { action_window = std::move(value); });
    transport_ptr->response_body =
        R"({"businessEventId":"schedule-reminder-task-10","status":"accepted","deliveries":[],"actionStream":{"reminderTriggerId":"timing-1","expiresAt":"2026-08-03T00:10:00.000Z"}})";
    Schedule schedule{.id = 1, .event = "喝水", .start_time = At(2'000'000'000), .status = ScheduleStatus::kActive};
    ScheduleReminderTask task{.id = 10,
                              .schedule_id = 1,
                              .chain_id = 20,
                              .attempt = 1,
                              .timing_task_id = "timing-1",
                              .trigger_at = At(2'000'000'000),
                              .business_status = ScheduleReminderBusinessStatus::kWaitingAcknowledgement,
                              .timer_status = ScheduleReminderTimerStatus::kTriggered,
                              .triggered_at = At(2'000'000'001),
                              .created_at = At(1'999'999'000),
                              .updated_at = At(2'000'000'001)};
    Check(notification.SendScheduleReminder(schedule, task).ok(), "提醒通知应提交到 IM 公共接口");
    Check(transport_ptr->requests.back().body.find("schedule-reminder-device-device-1-task-10") != std::string::npos,
          "提醒业务键应包含设备标识，避免不同设备的本地任务 ID 冲突");
    Check(action_window.has_value() && action_window->reminderTriggerId == "timing-1", "强提醒响应应发布动作窗口");

    auto final_task = task;
    final_task.attempt = 3;
    transport_ptr->response_body =
        R"({"businessEventId":"schedule-reminder-task-10-final","status":"accepted","deliveries":[],"actionStream":{"reminderTriggerId":"timing-1","expiresAt":"2026-08-03T00:10:00.000Z"}})";
    Check(notification.SendScheduleReminder(schedule, final_task).ok(), "第三次提醒通知应提交到 IM 公共接口");
    Check(transport_ptr->requests.back().body.find("schedule-reminder-device-device-1-task-10") != std::string::npos,
          "同一提醒任务重试应复用稳定的设备作用域业务键");
    Check(transport_ptr->requests.back().body.find("这是最后一次提醒；之后不再创建新的推迟提醒。") != std::string::npos,
          "第三次 IM 提醒正文应追加最后一次稍后提醒说明");

    InMemoryScheduleRepository schedules({schedule});
    voicelife::storage_memory::MemoryScheduleReminderTaskRepository reminders;
    Check(reminders.Insert(task).ok(), "动作测试应保存已触发任务");
    auto follow_up = task;
    follow_up.id = 0;
    follow_up.attempt = 2;
    follow_up.timing_task_id = "timing-2";
    follow_up.trigger_at = At(2'000'000'600);
    follow_up.business_status = ScheduleReminderBusinessStatus::kScheduled;
    follow_up.timer_status = ScheduleReminderTimerStatus::kPending;
    follow_up.triggered_at = std::nullopt;
    Check(reminders.Insert(follow_up).ok(), "动作测试应保存默认后续任务");
    Rules rules;
    Exceptions exceptions;
    ScheduleRuleService rule_service(rules, exceptions, schedules);
    ScheduleService schedule_service(schedules);
    Timing timing;
    Speech speech;
    ScheduleReminderService reminder_service(schedules, reminders, schedule_service, rule_service, timing, speech,
                                             nullptr, [] { return At(2'000'000'100); });
    ImScheduleReminderActionExecutor executor(reminder_service);

    ReminderActionCommand snooze;
    snooze.schemaVersion = "1";
    snooze.operationId = "operation-snooze";
    snooze.reminderTriggerId = "timing-1";
    snooze.action = "snooze";
    snooze.minutes = 10;
    const auto snoozed = executor.Execute(snooze);
    Check(snoozed.status == "succeeded" && snoozed.nextTriggerAt.has_value() && timing.cancel_count == 0,
          "IM 延迟动作应返回已持久化的下一次提醒时间且不重建默认后续提醒");
    const auto snoozed_tasks = reminders.FindBySchedule(1);
    Check(snoozed_tasks.ok() &&
              snoozed_tasks.value->front().business_status == ScheduleReminderBusinessStatus::kSnoozed &&
              snoozed_tasks.value->front().action_operation_id == "operation-snooze",
          "IM 延迟动作应只把目标 ReminderTrigger 持久化为 snoozed");

    ImScheduleReminderActionExecutor restarted_executor(reminder_service);
    const auto replayed_snooze = restarted_executor.Execute(snooze);
    Check(replayed_snooze.status == "succeeded" && replayed_snooze.nextTriggerAt == snoozed.nextTriggerAt &&
              timing.cancel_count == 0,
          "执行器重建后相同 operationId 应复用持久化结果而不是再次执行动作");

    Schedule acknowledged_schedule = schedule;
    acknowledged_schedule.id = 2;
    acknowledged_schedule.event = "吃药";
    Check(schedules.Insert(acknowledged_schedule).ok(), "确认动作测试应保存独立日程");
    auto acknowledged_task = task;
    acknowledged_task.id = 0;
    acknowledged_task.schedule_id = 2;
    acknowledged_task.chain_id = 30;
    acknowledged_task.timing_task_id = "timing-ack-1";
    Check(reminders.Insert(acknowledged_task).ok(), "确认动作测试应保存目标提醒");
    auto acknowledged_follow_up = follow_up;
    acknowledged_follow_up.id = 0;
    acknowledged_follow_up.schedule_id = 2;
    acknowledged_follow_up.chain_id = 30;
    acknowledged_follow_up.timing_task_id = "timing-ack-2";
    Check(reminders.Insert(acknowledged_follow_up).ok(), "确认动作测试应保存目标后续提醒");

    ReminderActionCommand acknowledge = snooze;
    acknowledge.reminderTriggerId = "timing-ack-1";
    acknowledge.action = "acknowledge";
    acknowledge.minutes = std::nullopt;
    const auto conflicting_operation = executor.Execute(acknowledge);
    Check(conflicting_operation.status == "failed" && timing.cancel_count == 0 &&
              schedules.FindById(2).value->status == ScheduleStatus::kActive,
          "其他提醒不得复用已有 operationId，且冲突不得产生业务副作用");

    acknowledge.operationId = "operation-ack";
    const auto acknowledged = executor.Execute(acknowledge);
    Check(acknowledged.status == "succeeded" && timing.cancel_count == 1, "IM 确认动作应只取消目标链的默认后续提醒");
    Check(schedules.FindById(1).value->status == ScheduleStatus::kActive &&
              schedules.FindById(2).value->status == ScheduleStatus::kCompleted,
          "IM 确认动作应按 reminderTriggerId 完成关联日程而不影响其他提醒");

    ReminderActionCommand invalid_snooze = snooze;
    invalid_snooze.operationId = "operation-invalid-snooze";
    invalid_snooze.reminderTriggerId = "timing-ack-1";
    invalid_snooze.minutes = 5;
    Check(executor.Execute(invalid_snooze).status == "failed", "非 10 分钟延迟参数应被业务执行器拒绝");
    return 0;
}
