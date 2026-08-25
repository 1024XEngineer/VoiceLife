#include "voicelife/im/im_action_channel.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "im_wire.h"
#include "voicelife/contracts/im/im_contracts.h"
#include "voicelife/contracts/im/notification_submission.h"
#include "voicelife/contracts/json.h"

namespace voicelife::im {
namespace {

using contracts::im::NotificationSubmission;
using contracts::im::ReminderActionCommand;
using contracts::im::ReminderActionResult;

constexpr int64_t kMsPerSecond = 1000;
constexpr int64_t kMsPerMinute = 60 * kMsPerSecond;
constexpr int64_t kMsPerDay = 24 * 60 * kMsPerMinute;

// ISO-8601（Z 或 ±HH:MM，任意小数精度）解析为自 epoch 起的毫秒数。
// 格式已在契约层校验；解析失败返回 nullopt，调用方按已过期保守处理。
std::optional<int64_t> IsoToEpochMillis(std::string_view iso) {
    size_t pos = 0;
    auto read_digits = [&](size_t count) -> std::optional<int64_t> {
        if (pos + count > iso.size()) {
            return std::nullopt;
        }
        int64_t value = 0;
        for (size_t i = 0; i < count; ++i) {
            const char current = iso[pos + i];
            if (current < '0' || current > '9') {
                return std::nullopt;
            }
            value = value * 10 + (current - '0');
        }
        pos += count;
        return value;
    };
    auto expect = [&](char expected) -> bool {
        if (pos >= iso.size() || iso[pos] != expected) {
            return false;
        }
        ++pos;
        return true;
    };

    const auto year = read_digits(4);
    if (!year.has_value() || !expect('-')) {
        return std::nullopt;
    }
    const auto month = read_digits(2);
    if (!month.has_value() || !expect('-')) {
        return std::nullopt;
    }
    const auto day = read_digits(2);
    if (!day.has_value() || !expect('T')) {
        return std::nullopt;
    }
    const auto hour = read_digits(2);
    if (!hour.has_value() || !expect(':')) {
        return std::nullopt;
    }
    const auto minute = read_digits(2);
    if (!minute.has_value() || !expect(':')) {
        return std::nullopt;
    }
    const auto second = read_digits(2);
    if (!second.has_value()) {
        return std::nullopt;
    }

    // 小数部分截断到毫秒；超出毫秒的位与秒级有效期比较可忽略。
    int64_t millis = 0;
    if (pos < iso.size() && iso[pos] == '.') {
        ++pos;
        int digits = 0;
        while (pos < iso.size() && iso[pos] >= '0' && iso[pos] <= '9') {
            if (digits < 3) {
                millis = millis * 10 + (iso[pos] - '0');
            }
            ++digits;
            ++pos;
        }
        if (digits < 1) {
            return std::nullopt;
        }
    }

    // 时区偏移换算到 UTC。
    int64_t offset_minutes = 0;
    if (pos < iso.size() && iso[pos] == 'Z') {
        ++pos;
    } else if (pos < iso.size() && (iso[pos] == '+' || iso[pos] == '-')) {
        const char sign = iso[pos];
        ++pos;
        const auto offset_hour = read_digits(2);
        if (!offset_hour.has_value() || !expect(':')) {
            return std::nullopt;
        }
        const auto offset_minute = read_digits(2);
        if (!offset_minute.has_value()) {
            return std::nullopt;
        }
        offset_minutes = *offset_hour * 60 + *offset_minute;
        if (sign == '-') {
            offset_minutes = -offset_minutes;
        }
    } else {
        return std::nullopt;
    }
    if (pos != iso.size()) {
        return std::nullopt;
    }

    // 公历日期到 epoch 天数（Howard Hinnant 算法）。
    int64_t adjusted_year = *year;
    unsigned adjusted_month = static_cast<unsigned>(*month);
    if (adjusted_month <= 2) {
        adjusted_year -= 1;
        adjusted_month += 12;
    }
    const int64_t era = (adjusted_year >= 0 ? adjusted_year : adjusted_year - 399) / 400;
    const unsigned year_of_era = static_cast<unsigned>(adjusted_year - era * 400);
    const unsigned day_of_year = (153 * (adjusted_month - 3) + 2) / 5 + static_cast<unsigned>(*day) - 1;
    const unsigned day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    const int64_t epoch_days = era * 146097 + static_cast<int64_t>(day_of_era) - 719468;

    const int64_t seconds_of_day = *hour * 3600 + *minute * 60 + *second;
    return epoch_days * kMsPerDay + seconds_of_day * kMsPerSecond + millis - offset_minutes * kMsPerMinute;
}

// 把 ISO 时间解析为 epoch 毫秒；解析失败视为已过期（保守拒绝）。
int64_t EpochMillisOrExpired(const std::string& iso) { return IsoToEpochMillis(iso).value_or(0); }

// 当前时刻的 epoch 毫秒；解析失败视为早已过期（保守拒绝）。
int64_t NowEpochMillis(const std::string& now_iso) { return IsoToEpochMillis(now_iso).value_or(INT64_MAX); }

// 幂等缓存键：{提醒触发, operationId}，避免跨窗口误判重放。
std::string IdempotencyKey(const std::string& trigger_id, const std::string& operation_id) {
    return trigger_id + '\x1f' + operation_id;
}

// 执行器结果必须能序列化并通过契约解析，否则视为本地故障。
bool ResultIsReportable(const ReminderActionResult& result) {
    voicelife::JsonValue root;
    ReminderActionResult validated;
    return voicelife::ParseJson(SerializeReminderActionResult(result), root).ok() &&
           ParseReminderActionResult(root, validated).ok();
}

}  // namespace

std::optional<ActionWindow> ExtractActionWindow(const std::string& submission_body) {
    voicelife::JsonValue root;
    NotificationSubmission submission;
    if (!voicelife::ParseJson(submission_body, root).ok() || !ParseNotificationSubmission(root, submission).ok() ||
        !submission.actionStream.has_value()) {
        return std::nullopt;
    }
    ActionWindow window;
    window.reminderTriggerId = submission.actionStream->reminderTriggerId;
    window.expiresAt = submission.actionStream->expiresAt;
    return window;
}

ImActionChannel::ImActionChannel(ImReportingChannel& reporting, ImCredentialProvider& credentials,
                                 ImActionExecutor& executor, ImClock& clock)
    : reporting_(reporting), credentials_(credentials), executor_(executor), clock_(clock) {}

ActionRunResult ImActionChannel::Run(ImActionCommandStream& stream, const ActionWindow& window) {
    ActionRunResult result;
    // 窗口在 expiresAt 时刻起失效，now == expiresAt 即视为过期，不得建流。
    const int64_t window_expires_ms = EpochMillisOrExpired(window.expiresAt);
    const int64_t now_ms = NowEpochMillis(clock_.NowIso());
    if (now_ms >= window_expires_ms) {
        result.status = ActionRunStatus::kWindowExpired;
        return result;
    }
    // 清理已过期窗口的执行结果缓存：窗口关闭后同 trigger 的新窗口应重新执行，
    // 且避免设备长期运行后缓存无界增长。
    for (auto it = executed_.begin(); it != executed_.end();) {
        if (it->second.window_expires_ms <= now_ms) {
            it = executed_.erase(it);
        } else {
            ++it;
        }
    }

    bool has_unconfirmed = false;
    // 连接建立失败不得与「正常空流结束」混淆，必须归类为可重连。
    if (!stream.Open(cursors_[window.reminderTriggerId])) {
        result.status = ActionRunStatus::kDisconnected;
        return result;
    }
    while (true) {
        // 每次读取前复查窗口：连接保持期间窗口过期必须中止，避免过期后仍执行命令。
        if (NowEpochMillis(clock_.NowIso()) >= window_expires_ms) {
            stream.Close();
            result.status = ActionRunStatus::kWindowExpired;
            return result;
        }
        const StreamRead read = stream.Next();
        if (read.status == StreamReadStatus::kEndOfStream) {
            // 服务端正常关闭连接：结果全部确认时正常结束。
            break;
        }
        if (read.status == StreamReadStatus::kNetworkError || read.status == StreamReadStatus::kProtocolError) {
            // 连接中断或协议错误：结果可能未确认，必须按可重连处理，调用方
            // 据此重连并重放未确认命令，不得误报正常结束。
            has_unconfirmed = true;
            break;
        }
        HandleCommand(read.command, window, result, has_unconfirmed);
        // 回执未被网关受理时立即释放当前 SSE。网关会在下一次连接中按
        // Last-Event-ID/operationId 重放未确认命令；继续等待当前流会与
        // 网关的 processing 状态互相等待，直到动作窗口过期。
        if (has_unconfirmed) break;
    }
    stream.Close();

    result.status = has_unconfirmed ? ActionRunStatus::kDisconnected : ActionRunStatus::kFinished;
    return result;
}

void ImActionChannel::HandleCommand(const ReminderActionCommand& command, const ActionWindow& window,
                                    ActionRunResult& result, bool& has_unconfirmed) {
    // 归属校验：非本设备或窗口外命令本地丢弃并推进游标，避免无限重放，
    // 同时计入 dropped 供调用方观测路由异常。
    if (command.deviceId != credentials_.DeviceId() || command.reminderTriggerId != window.reminderTriggerId) {
        cursors_[window.reminderTriggerId] = command.commandId;
        ++result.dropped;
        return;
    }

    // 过期命令：回传 expired 终态并确认，不执行本地动作。时区偏移等值
    // 已按 UTC 归一化，比较不受 ISO 字符串字面序影响。
    if (EpochMillisOrExpired(command.expiresAt) <= NowEpochMillis(clock_.NowIso())) {
        ReminderActionResult expired;
        expired.schemaVersion = contracts::im::kDeviceContractVersion;
        expired.operationId = command.operationId;
        expired.reminderTriggerId = command.reminderTriggerId;
        expired.status = "expired";
        expired.occurredAt = clock_.NowIso();
        const ReportResult report = reporting_.SubmitReminderActionResult(expired, command.deviceId, command.commandId);
        Settle(report, command, window.reminderTriggerId, result, has_unconfirmed);
        return;
    }

    // operationId 去重：相同操作只执行一次，重放复用缓存结果幂等回传。
    const std::string cache_key = IdempotencyKey(window.reminderTriggerId, command.operationId);
    const auto cached = executed_.find(cache_key);
    if (cached != executed_.end()) {
        const ReportResult report =
            reporting_.SubmitReminderActionResult(cached->second.result, command.deviceId, command.commandId);
        Settle(report, command, window.reminderTriggerId, result, has_unconfirmed);
        return;
    }

    ReminderActionResult outcome = executor_.Execute(command);
    // 结果身份字段以命令为准，保证回传幂等键与网关侧 operationId 一致。
    outcome.operationId = command.operationId;
    outcome.reminderTriggerId = command.reminderTriggerId;
    // 执行器结果必须先通过契约校验再缓存/推进：非法结果本地故障隔离，
    // 不缓存、不确认，网关保持重放机会，避免重复回传同一非法结果。
    if (!ResultIsReportable(outcome)) {
        has_unconfirmed = true;
        return;
    }
    executed_[cache_key] = CachedExecution{EpochMillisOrExpired(window.expiresAt), outcome};
    ++result.executed;
    const ReportResult report = reporting_.SubmitReminderActionResult(outcome, command.deviceId, command.commandId);
    Settle(report, command, window.reminderTriggerId, result, has_unconfirmed);
}

void ImActionChannel::Settle(const ReportResult& report, const ReminderActionCommand& command,
                             const std::string& trigger_id, ActionRunResult& result, bool& has_unconfirmed) {
    // 仅网关明确受理结果（kSubmitted）才推进确认游标；可重试、凭据被拒与
    // 明确拒绝均非业务 ACK，网关需在重连后重放该命令。
    if (report.status == ReportStatus::kSubmitted) {
        cursors_[trigger_id] = command.commandId;
        ++result.confirmed;
        return;
    }
    has_unconfirmed = true;
}

}  // namespace voicelife::im
