#define main ExistingScheduleReminderImAdapterTestMain
#include "schedule_reminder_im_adapter_test.cc"
#undef main

namespace {

/** 支持配置 HTTP 提交结果的 IM 传输替身。 */
class ConfigurableTransport final : public ImTransport {
   public:
    /**
     * @brief 返回预设的通知提交响应。
     * @param request IM HTTP 请求。
     * @return 预设响应。
     */
    ImHttpResponse Post(const ImHttpRequest& request) override {
        requests.push_back(request);
        return response;
    }

    /**
     * @brief 返回成功的网关探针响应。
     * @param request IM HTTP 请求。
     * @return 成功响应。
     */
    ImHttpResponse Get(const ImHttpRequest& request) override {
        requests.push_back(request);
        return {.status = ImTransportStatus::kSuccess, .status_code = 200, .body = {}, .message = {}};
    }

    ImHttpResponse response;
    std::vector<ImHttpRequest> requests;
};

/**
 * @brief 创建用于通知适配器测试的日程。
 * @return 活动日程。
 */
Schedule AdapterSchedule() {
    return {.id = 1,
            .event = "失败路径提醒",
            .start_time = At(2'000'000'000),
            .end_time = std::nullopt,
            .location = std::nullopt,
            .notes = std::nullopt,
            .rule_id = std::nullopt,
            .status = ScheduleStatus::kActive,
            .created_at = At(1'999'999'000),
            .updated_at = At(1'999'999'000)};
}

/**
 * @brief 创建用于通知适配器测试的提醒任务。
 * @return 已触发提醒任务。
 */
ScheduleReminderTask AdapterTask() {
    return {.id = 10,
            .schedule_id = 1,
            .chain_id = 20,
            .attempt = 1,
            .timing_task_id = std::nullopt,
            .trigger_at = At(2'000'000'000),
            .business_status = ScheduleReminderBusinessStatus::kWaitingAcknowledgement,
            .timer_status = ScheduleReminderTimerStatus::kTriggered,
            .triggered_at = std::nullopt,
            .created_at = At(1'999'999'000),
            .updated_at = At(2'000'000'001)};
}

/**
 * @brief 验证 IM Runtime 未就绪时通知适配器返回可重试错误。
 * @return 无。
 */
void CheckNotificationRejectsUnreadyRuntime() {
    RuntimeInputs inputs;
    ImRuntime runtime(inputs, inputs, inputs,
                      [](const std::string&) { return std::make_unique<ConfigurableTransport>(); });
    ImScheduleReminderNotification notification(runtime, {});
    const Status status = notification.SendScheduleReminder(AdapterSchedule(), AdapterTask());
    Check(!status.ok() && status.code == voicelife::ErrorCode::kUnavailable,
          "IM Runtime 未就绪时提醒通知应返回可重试错误");
}

/**
 * @brief 验证通知提交失败会映射为对应的领域错误和默认消息。
 * @return 无。
 */
void CheckNotificationFailureMappings() {
    RuntimeInputs inputs;
    ConfigurableTransport* transport = nullptr;
    ImRuntime runtime(inputs, inputs, inputs, [&transport](const std::string&) {
        auto created = std::make_unique<ConfigurableTransport>();
        transport = created.get();
        return created;
    });
    Check(runtime.Start().ok(), "通知失败映射测试应启动 IM Runtime");
    Check(runtime.ProbeGateway().status == ImTransportStatus::kSuccess, "通知失败映射测试应完成网关探针");
    ImScheduleReminderNotification notification(runtime, {});

    transport->response = {
        .status = ImTransportStatus::kNetworkFailure, .status_code = 0, .body = {}, .message = "网络暂不可用"};
    const Status retryable = notification.SendScheduleReminder(AdapterSchedule(), AdapterTask());
    Check(
        !retryable.ok() && retryable.code == voicelife::ErrorCode::kUnavailable && retryable.message == "网络暂不可用",
        "网络错误应保留消息并映射为可重试错误");

    transport->response = {.status = ImTransportStatus::kHttpError, .status_code = 400, .body = {}, .message = {}};
    const Status rejected = notification.SendScheduleReminder(AdapterSchedule(), AdapterTask());
    Check(
        !rejected.ok() && rejected.code == voicelife::ErrorCode::kInternal && rejected.message == "IM 提醒通知提交失败",
        "不可重试拒绝应映射为内部错误并提供默认消息");
}

/**
 * @brief 验证动作执行器对不支持和业务拒绝结果的映射。
 * @return 无。
 */
void CheckActionFailureMappings() {
    InMemoryScheduleRepository schedules({AdapterSchedule()});
    voicelife::storage_memory::MemoryScheduleReminderTaskRepository reminders;
    Rules rules;
    Exceptions exceptions;
    ScheduleRuleService rule_service(rules, exceptions, schedules);
    ScheduleService schedule_service(schedules);
    Timing timing;
    Speech speech;
    ScheduleReminderService reminder_service(schedules, reminders, schedule_service, rule_service, timing, speech,
                                             nullptr, [] { return At(2'000'000'100); });
    ImScheduleReminderActionExecutor executor(reminder_service);
    ReminderActionCommand command;
    command.schemaVersion = "1";
    command.operationId = "operation-failure";
    command.reminderTriggerId = "missing-trigger";

    command.action = "dismiss";
    const auto unsupported = executor.Execute(command);
    Check(unsupported.status == "failed" && unsupported.errorCode == "unsupported_action", "未知动作应返回不支持错误");

    command.action = "acknowledge";
    const auto rejected = executor.Execute(command);
    Check(rejected.status == "failed" && rejected.errorCode == "reminder_action_rejected",
          "没有最近提醒时确认动作应返回业务拒绝");
}

}  // namespace

int main() {
    CheckNotificationRejectsUnreadyRuntime();
    CheckNotificationFailureMappings();
    CheckActionFailureMappings();
    return 0;
}
