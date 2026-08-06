#pragma once

#include <optional>
#include <string>

#include "im_contracts.h"
#include "voicelife/contracts/json.h"

namespace voicelife::contracts::im {

/// 网关通过 SSE 下发给设备执行的提醒动作命令。
struct ReminderActionCommand {
    std::string schemaVersion;
    std::string commandId;
    std::string operationId;
    std::string correlationId;
    std::string deviceId;
    std::string actorBindingId;
    std::string reminderTriggerId;
    std::string action;          ///< "acknowledge" 或 "snooze"。
    std::optional<int> minutes;  ///< snooze 动作必需的推迟分钟数（params.minutes）。
    std::string occurredAt;
    std::string expiresAt;
};

/**
 * @brief 解析并校验 ReminderActionCommand。
 * @param root 已解析的 JSON 对象（SSE 帧的 data 字段）。
 * @param out 解析成功后的动作命令。
 * @return 契约非法时返回 kInvalidArgument，语义与 TypeScript parseReminderActionCommand 一致。
 */
Status ParseReminderActionCommand(const JsonValue& root, ReminderActionCommand& out);

}  // namespace voicelife::contracts::im
