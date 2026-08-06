#include "voicelife/im/im_action_channel.h"

#include <optional>
#include <string>
#include <utility>

#include "im_wire.h"
#include "voicelife/contracts/im/im_contracts.h"

namespace voicelife::im {
namespace {

using contracts::im::ReminderActionCommand;
using contracts::im::ReminderActionResult;

// ISO-8601 UTC（固定毫秒精度 + Z）按字典序比较即时间序。
bool LaterThan(const std::string& left, const std::string& right) { return left > right; }

// 命令在 expiresAt 时刻起失效。
bool IsExpired(const std::string& expires_at, const std::string& now) { return expires_at <= now; }

}  // namespace

ImActionChannel::ImActionChannel(ImReportingChannel& reporting, ImCredentialProvider& credentials,
                                 ImActionExecutor& executor, ImClock& clock)
    : reporting_(reporting), credentials_(credentials), executor_(executor), clock_(clock) {}

ActionRunResult ImActionChannel::Run(ImActionCommandStream& stream, const ActionWindow& window) {
    ActionRunResult result;
    if (LaterThan(clock_.NowIso(), window.expiresAt)) {
        result.status = ActionRunStatus::kWindowExpired;
        return result;
    }

    bool has_unconfirmed = false;
    stream.Open(last_confirmed_command_id_);
    while (true) {
        const std::optional<ReminderActionCommand> next = stream.Next();
        if (!next.has_value()) {
            break;
        }
        HandleCommand(*next, window, result, has_unconfirmed);
    }
    stream.Close();

    result.status = has_unconfirmed ? ActionRunStatus::kDisconnected : ActionRunStatus::kFinished;
    return result;
}

void ImActionChannel::HandleCommand(const ReminderActionCommand& command, const ActionWindow& window,
                                    ActionRunResult& result, bool& has_unconfirmed) {
    // 归属校验：非本设备或窗口外命令本地丢弃并推进游标，避免无限重放。
    if (command.deviceId != credentials_.DeviceId()) {
        last_confirmed_command_id_ = command.commandId;
        return;
    }
    if (command.reminderTriggerId != window.reminderTriggerId) {
        last_confirmed_command_id_ = command.commandId;
        return;
    }

    // 过期命令：回传 expired 终态并确认，不执行本地动作。
    if (IsExpired(command.expiresAt, clock_.NowIso())) {
        ReminderActionResult expired;
        expired.schemaVersion = contracts::im::kDeviceContractVersion;
        expired.operationId = command.operationId;
        expired.reminderTriggerId = command.reminderTriggerId;
        expired.status = "expired";
        expired.occurredAt = clock_.NowIso();
        const ReportResult report = reporting_.SubmitReminderActionResult(expired, command.deviceId, command.commandId);
        Settle(report, command, result, has_unconfirmed);
        return;
    }

    // operationId 去重：相同操作只执行一次，重放复用缓存结果幂等回传。
    const auto cached = executed_.find(command.operationId);
    if (cached != executed_.end()) {
        const ReportResult report =
            reporting_.SubmitReminderActionResult(cached->second, command.deviceId, command.commandId);
        Settle(report, command, result, has_unconfirmed);
        return;
    }

    ReminderActionResult outcome = executor_.Execute(command);
    // 结果身份字段以命令为准，保证回传幂等键与网关侧 operationId 一致。
    outcome.operationId = command.operationId;
    outcome.reminderTriggerId = command.reminderTriggerId;
    executed_[command.operationId] = outcome;
    ++result.executed;
    const ReportResult report = reporting_.SubmitReminderActionResult(outcome, command.deviceId, command.commandId);
    Settle(report, command, result, has_unconfirmed);
}

void ImActionChannel::Settle(const ReportResult& report, const ReminderActionCommand& command, ActionRunResult& result,
                             bool& has_unconfirmed) {
    if (report.status == ReportStatus::kRetryable) {
        // 未确认：游标不推进，重连后网关重放该命令。
        has_unconfirmed = true;
        return;
    }
    last_confirmed_command_id_ = command.commandId;
    if (report.status == ReportStatus::kSubmitted) {
        ++result.confirmed;
    }
}

}  // namespace voicelife::im
