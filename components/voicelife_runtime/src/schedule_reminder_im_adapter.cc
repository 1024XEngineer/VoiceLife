#include "schedule_reminder_im_adapter.h"

#include <chrono>
#include <ctime>
#include <string>
#include <utility>

#include "voicelife/contracts/im/im_contracts.h"
#include "voicelife/im/im_reporting_channel.h"

#ifdef ESP_PLATFORM
#include "esp_log.h"
#endif

namespace voicelife::runtime {
namespace {

#ifdef ESP_PLATFORM
constexpr char kLogTag[] = "VoiceLifeReminderIm";
#endif

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
                                                 std::string status, std::string error_code, std::string occurred_at,
                                                 std::optional<std::string> next_trigger_at = std::nullopt) {
    contracts::im::ReminderActionResult result;
    result.schemaVersion = contracts::im::kDeviceContractVersion;
    result.operationId = command.operationId;
    result.reminderTriggerId = command.reminderTriggerId;
    result.status = std::move(status);
    result.nextTriggerAt = std::move(next_trigger_at);
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
    // Task IDs are local SQLite row IDs and can be reused after a database
    // rebuild. Include the provisioned device identity in the global
    // idempotency key while keeping it stable across retries.
    intent.businessEventId = "schedule-reminder-device-" + device_id + "-task-" + DecimalId(task.id);
    intent.correlationId = "schedule-reminder-chain-" + DecimalId(task.chain_id);
    intent.kind = "reminder_due";
    intent.recipient = {.userId = user_id, .deviceId = device_id};
    intent.scheduleId = DecimalId(schedule.id);
    intent.taskId = DecimalId(task.id);
    intent.instanceId = DecimalId(schedule.id);
    intent.reminderTriggerId = task.timing_task_id.value_or("schedule-reminder-task-" + DecimalId(task.id));
    intent.reminderType = "strong";
    intent.content = {
        .title = "日程提醒",
        .body = schedule.event + (task.attempt >= 3 ? "\n这是最后一次提醒；之后不再创建新的推迟提醒。" : "")};
    intent.actions = {
        {.kind = "command", .type = "acknowledge", .label = "知道了", .minutes = std::nullopt},
        {.kind = "command", .type = "snooze", .label = "推迟 10 分钟", .minutes = 10},
    };
    intent.plannedAt = FormatIso(task.trigger_at);
    intent.triggerAt = FormatIso(task.trigger_at);
    intent.occurredAt = FormatIso(task.triggered_at.value_or(task.trigger_at));

    const im::ReportResult result = reporting->SubmitNotification(intent);
#ifdef ESP_PLATFORM
    ESP_LOGI(kLogTag, "IM_REMINDER_SUBMIT_RESULT status=%d response_bytes=%u correlation_id=%s reminder_trigger_id=%s",
             static_cast<int>(result.status), static_cast<unsigned>(result.response_body.size()),
             intent.correlationId.c_str(), intent.reminderTriggerId.c_str());
#endif
    if (result.status == im::ReportStatus::kSubmitted) {
        auto window = im::ExtractActionWindow(result.response_body);
#ifdef ESP_PLATFORM
        if (window.has_value()) {
            ESP_LOGI(kLogTag, "IM_ACTION_WINDOW_ACCEPTED=1 reminder_trigger_id=%s expires_at=%s",
                     window->reminderTriggerId.c_str(), window->expiresAt.c_str());
        } else {
            ESP_LOGW(kLogTag, "IM_ACTION_WINDOW_ACCEPTED=0 reminder_trigger_id=%s", intent.reminderTriggerId.c_str());
        }
#endif
        if (action_window_sink_) {
            if (window.has_value()) action_window_sink_(std::move(*window));
        }
        return Status::Ok();
    }
#ifdef ESP_PLATFORM
    ESP_LOGW(kLogTag, "IM_REMINDER_SUBMIT_FAILED=1 status=%d correlation_id=%s reminder_trigger_id=%s",
             static_cast<int>(result.status), intent.correlationId.c_str(), intent.reminderTriggerId.c_str());
#endif
    const ErrorCode code =
        result.status == im::ReportStatus::kRetryable ? ErrorCode::kUnavailable : ErrorCode::kInternal;
    return Status::Error(code, result.message.empty() ? "IM 提醒通知提交失败" : result.message);
}

contracts::im::ReminderActionResult ImScheduleReminderActionExecutor::Execute(
    const contracts::im::ReminderActionCommand& command) {
    schedule::ReminderActionCommand local;
    local.operation_id = command.operationId;
    local.reminder_trigger_id = command.reminderTriggerId;
    if (command.action == "acknowledge") {
        local.action = schedule::ScheduleReminderActionKind::kAcknowledge;
    } else if (command.action == "snooze") {
        local.action = schedule::ScheduleReminderActionKind::kSnooze;
        local.snooze_minutes = command.minutes;
    } else {
        return ActionResult(command, "failed", "unsupported_action", EspScheduleReminderClock{}.NowIso());
    }
    const auto result = service_.ExecuteReminderAction(local);
    if (result.ok()) {
        return ActionResult(command, "succeeded", {}, FormatIso(result.value->occurred_at),
                            result.value->next_trigger_at.has_value()
                                ? std::optional<std::string>{FormatIso(*result.value->next_trigger_at)}
                                : std::nullopt);
    }
    const std::string occurred_at = EspScheduleReminderClock{}.NowIso();
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
