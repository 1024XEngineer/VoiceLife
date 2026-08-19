#include "voicelife/reminder_im/im_reminder_bridge.h"

#include <ctime>
#include <thread>
#include <utility>

#include "voicelife/contracts/im/notification_intent.h"
#include "voicelife/im/im_action_channel.h"

namespace voicelife::reminder_im {
namespace {

/// 将秒精度时间格式化为网关联约接受的 ISO-8601 UTC（毫秒精度固定为零）。
std::string FormatIso8601Utc(schedule::DateTime time) {
    const std::time_t seconds = std::chrono::system_clock::to_time_t(time);
    std::tm utc{};
    gmtime_r(&seconds, &utc);
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &utc);
    return std::string(buffer) + ".000Z";
}

}  // namespace

ImReminderBridge::ImReminderBridge(im::ImRuntime& runtime, im::ImCredentialProvider& credentials, im::ImClock& clock,
                                   schedule::ScheduleReminderService* reminder_service,
                                   ReminderActionStreamFactory stream_factory, std::chrono::milliseconds reconnect_delay)
    : runtime_(runtime),
      credentials_(credentials),
      clock_(clock),
      reminder_service_(reminder_service),
      stream_factory_(std::move(stream_factory)),
      reconnect_delay_(reconnect_delay) {}

void ImReminderBridge::NotifyReminderFired(const schedule::ReminderFireNotice& notice) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.size() >= kQueueCapacity) {
        // 定时器回调上下文：队列满时静默丢弃，避免阻塞提醒服务。
        dropped_fires_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    queue_.push_back(notice);
    cv_.notify_one();
}

bool ImReminderBridge::WaitForItem(schedule::ReminderFireNotice& out) {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
    if (queue_.empty()) return false;
    out = std::move(queue_.front());
    queue_.pop_front();
    return true;
}

void ImReminderBridge::RunWorkerLoop() {
    schedule::ReminderFireNotice notice;
    while (WaitForItem(notice)) {
        HandleFire(notice);
    }
}

void ImReminderBridge::RequestStop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
}

bool ImReminderBridge::IsStopRequested() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stop_;
}

void ImReminderBridge::SetScheduleReminderService(schedule::ScheduleReminderService* service) {
    reminder_service_ = service;
}

void ImReminderBridge::HandleFire(const schedule::ReminderFireNotice& notice) {
    // 降级路径：任一前置不满足都直接跳过本轮通知，语音提醒不受影响。
    if (runtime_.state() != im::ImRuntimeState::kReady) return;
    im::ImReportingChannel* reporting = runtime_.reporting_channel();
    if (reporting == nullptr || !runtime_.user_id().has_value()) return;

    contracts::im::NotificationIntent intent;
    intent.schemaVersion = "1";
    intent.kind = "reminder_due";
    intent.reminderType = "strong";
    const std::string trigger =
        "reminder-" + std::to_string(notice.schedule_id) + "-" + std::to_string(notice.task_id);
    intent.businessEventId = trigger;
    intent.correlationId = trigger;
    intent.reminderTriggerId = trigger;
    intent.recipient.userId = *runtime_.user_id();
    intent.recipient.deviceId = runtime_.device_id();
    intent.scheduleId = std::to_string(notice.schedule_id);
    intent.taskId = std::to_string(notice.task_id);
    intent.instanceId = std::to_string(notice.schedule_id);
    intent.content.title = notice.event;
    intent.content.body = "提醒：现在是「" + notice.event + "」时间了";
    intent.actions.push_back({.kind = "command", .type = "acknowledge", .label = "知道了", .minutes = std::nullopt});
    intent.actions.push_back({.kind = "command", .type = "snooze", .label = "推迟 10 分钟", .minutes = 10});
    intent.plannedAt = FormatIso8601Utc(notice.trigger_time);
    intent.triggerAt = FormatIso8601Utc(notice.trigger_time);
    intent.occurredAt = clock_.NowIso();

    const im::ReportResult report = reporting->SubmitNotification(intent);
    if (report.status != im::ReportStatus::kSubmitted) return;
    const std::optional<im::ActionWindow> window = im::ExtractActionWindow(report.response_body);
    if (!window.has_value()) return;
    // 窗口期间记住日程归属：命令不含 scheduleId，回传结果必须按窗口映射。
    active_window_ = ActiveWindow{window->reminderTriggerId, notice.schedule_id};
    RunActionWindow(*window);
    active_window_.reset();
}

void ImReminderBridge::RunActionWindow(const im::ActionWindow& window) {
    im::ImReportingChannel* reporting = runtime_.reporting_channel();
    if (reporting == nullptr) return;
    // 重连复用同一通道实例，保留 operationId 去重与游标状态。
    im::ImActionChannel channel(*reporting, credentials_, *this, clock_);
    for (;;) {
        std::unique_ptr<im::ImActionCommandStream> stream = stream_factory_(window.reminderTriggerId);
        if (!stream) return;
        const im::ActionRunResult run = channel.Run(*stream, window);
        // kDisconnected 是唯一可重试的终止：网络层失败按退避重连，其余终止（完成、
        // 窗口过期、执行器失败）都结束本窗口。停止请求不阻止处理已入队的窗口——
        // 生产环境的读取由传输层停止检查中断，重连睡眠由下方分段检查中断。
        if (run.status != im::ActionRunStatus::kDisconnected) return;
        // 分段睡眠等待退避，期间响应停止请求，使停止延迟有界。
        const auto deadline = std::chrono::steady_clock::now() + reconnect_delay_;
        while (std::chrono::steady_clock::now() < deadline) {
            if (IsStopRequested()) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
}

contracts::im::ReminderActionResult ImReminderBridge::Execute(
    const contracts::im::ReminderActionCommand& command) {
    contracts::im::ReminderActionResult result;
    result.schemaVersion = "1";
    result.operationId = command.operationId;
    result.reminderTriggerId = command.reminderTriggerId;
    result.occurredAt = clock_.NowIso();

    if (!active_window_.has_value() || active_window_->reminder_trigger_id != command.reminderTriggerId) {
        result.status = "failed";
        result.errorCode = "unknown_window";
        return result;
    }
    if (reminder_service_ == nullptr) {
        result.status = "failed";
        result.errorCode = "not_ready";
        return result;
    }
    if (command.action == "acknowledge") {
        const Status status = reminder_service_->AcknowledgeScheduleReminder(active_window_->schedule_id);
        if (status.ok()) {
            result.status = "succeeded";
            return result;
        }
        result.status = "failed";
        result.errorCode = "schedule_unavailable";
        return result;
    }
    if (command.action == "snooze") {
        const Result<schedule::DateTime> trigger =
            reminder_service_->SnoozeScheduleReminder(active_window_->schedule_id);
        if (trigger.ok()) {
            result.status = "succeeded";
            result.nextTriggerAt = FormatIso8601Utc(*trigger.value);
            return result;
        }
        result.status = "failed";
        result.errorCode =
            trigger.status.code == voicelife::ErrorCode::kConflict ? "snooze_limit" : "schedule_unavailable";
        return result;
    }
    result.status = "failed";
    result.errorCode = "unsupported_action";
    return result;
}

}  // namespace voicelife::reminder_im
