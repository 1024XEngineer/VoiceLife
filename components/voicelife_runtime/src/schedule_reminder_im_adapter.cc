#include "schedule_reminder_im_adapter.h"

#include <chrono>
#include <ctime>
#include <string>
#include <utility>

#include "voicelife/contracts/im/im_contracts.h"
#include "voicelife/im/im_reporting_channel.h"

namespace voicelife::runtime {
namespace {

std::string FormatIso(schedule::DateTime value) {
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(value.time_since_epoch()).count();
    const std::time_t timestamp = static_cast<std::time_t>(seconds);
    std::tm utc{};
#if defined(_WIN32)
    if (gmtime_s(&utc, &timestamp) != 0) return "1970-01-01T00:00:00.000Z";
#else
    if (gmtime_r(&timestamp, &utc) == nullptr) return "1970-01-01T00:00:00.000Z";
#endif
    char buffer[32]{};
    if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &utc) == 0) {
        return "1970-01-01T00:00:00.000Z";
    }
    return std::string(buffer) + ".000Z";
}

std::string DecimalId(int64_t value) { return std::to_string(value); }

contracts::im::ReminderActionResult ActionResult(const contracts::im::ReminderActionCommand& command,
                                                 std::string status, std::string error_code, std::string occurred_at) {
    contracts::im::ReminderActionResult result;
    result.schemaVersion = contracts::im::kDeviceContractVersion;
    result.operationId = command.operationId;
    result.reminderTriggerId = command.reminderTriggerId;
    result.status = std::move(status);
    if (!error_code.empty()) result.errorCode = std::move(error_code);
    result.occurredAt = std::move(occurred_at);
    return result;
}

}  // namespace

Status ImScheduleReminderNotification::SendScheduleReminder(const schedule::Schedule& schedule,
                                                            const schedule::ScheduleReminderTask& task) {
    im::ImReportingChannel* reporting = runtime_.reporting_channel();
    if (reporting == nullptr || runtime_.state() != im::ImRuntimeState::kReady) {
        return Status::Error(ErrorCode::kUnavailable, "IM Runtime 尚未就绪");
    }
    const std::string device_id = runtime_.device_id();
    const std::string user_id = runtime_.user_id().value_or("");
    if (device_id.empty() || user_id.empty()) {
        return Status::Error(ErrorCode::kUnavailable, "IM 收件人身份不完整");
    }

    contracts::im::NotificationIntent intent;
    intent.schemaVersion = contracts::im::kDeviceContractVersion;
    intent.businessEventId = "schedule-reminder-task-" + DecimalId(task.id);
    intent.correlationId = "schedule-reminder-chain-" + DecimalId(task.chain_id);
    intent.kind = "reminder_due";
    intent.recipient = {.userId = user_id, .deviceId = device_id};
    intent.scheduleId = DecimalId(schedule.id);
    intent.taskId = DecimalId(task.id);
    intent.instanceId = DecimalId(schedule.id);
    intent.reminderTriggerId = task.timing_task_id.value_or("schedule-reminder-task-" + DecimalId(task.id));
    intent.reminderType = "strong";
    intent.content = {.title = "日程提醒",
                      .body = schedule.event + (task.attempt >= 3
                                                     ? "\n这是最后一次提醒；之后不再创建新的推迟提醒。"
                                                     : "")};
    intent.actions = {
        {.kind = "command", .type = "acknowledge", .label = "知道了", .minutes = std::nullopt},
        {.kind = "command", .type = "snooze", .label = "推迟 10 分钟", .minutes = 10},
    };
    intent.plannedAt = FormatIso(task.trigger_at);
    intent.triggerAt = FormatIso(task.trigger_at);
    intent.occurredAt = FormatIso(task.triggered_at.value_or(task.trigger_at));

    const im::ReportResult result = reporting->SubmitNotification(intent);
    if (result.status == im::ReportStatus::kSubmitted) {
        if (action_window_sink_) {
            auto window = im::ExtractActionWindow(result.response_body);
            if (window.has_value()) action_window_sink_(std::move(*window));
        }
        return Status::Ok();
    }
    const ErrorCode code =
        result.status == im::ReportStatus::kRetryable ? ErrorCode::kUnavailable : ErrorCode::kInternal;
    return Status::Error(code, result.message.empty() ? "IM 提醒通知提交失败" : result.message);
}

contracts::im::ReminderActionResult ImScheduleReminderActionExecutor::Execute(
    const contracts::im::ReminderActionCommand& command) {
    const std::string occurred_at = EspScheduleReminderClock{}.NowIso();
    Result<schedule::ReminderActionResult> result;
    if (command.action == "acknowledge") {
        result = service_.AcknowledgeRecentReminders();
    } else if (command.action == "snooze") {
        result = service_.SnoozeRecentReminders();
    } else {
        return ActionResult(command, "failed", "unsupported_action", occurred_at);
    }
    if (result.ok()) return ActionResult(command, "succeeded", {}, occurred_at);
    if (result.status.code == ErrorCode::kUnavailable) {
        return ActionResult(command, "retryable_failed", "unavailable", occurred_at);
    }
    return ActionResult(command, "failed", "reminder_action_rejected", occurred_at);
}

std::string EspScheduleReminderClock::NowIso() {
    const std::time_t timestamp = std::time(nullptr);
    std::tm utc{};
#if defined(_WIN32)
    if (gmtime_s(&utc, &timestamp) != 0) return "1970-01-01T00:00:00.000Z";
#else
    if (gmtime_r(&timestamp, &utc) == nullptr) return "1970-01-01T00:00:00.000Z";
#endif
    char buffer[32]{};
    if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &utc) == 0) {
        return "1970-01-01T00:00:00.000Z";
    }
    return std::string(buffer) + ".000Z";
}

}  // namespace voicelife::runtime
