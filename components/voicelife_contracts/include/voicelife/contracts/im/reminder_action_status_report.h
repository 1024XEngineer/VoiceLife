#pragma once

#include <optional>
#include <string>

#include "im_contracts.h"
#include "voicelife/contracts/json.h"

namespace voicelife::contracts::im {

/// 设备语音动作完成后的独立业务事实，不绑定 Gateway commandId。
struct ReminderActionStatusReport {
    std::string schemaVersion;
    std::string eventId;
    std::string correlationId;
    std::string deviceId;
    std::string reminderTriggerId;
    std::string operationId;
    std::string action;  ///< acknowledge | snooze
    std::string status;  ///< succeeded | retryable_failed | failed | expired
    std::string occurredAt;
    std::optional<std::string> nextTriggerAt;
    std::optional<std::string> errorCode;
    std::optional<JsonValue> details;
    std::string source;  ///< voice
};

/**
 * @brief 解析并校验独立设备动作事实报告。
 * @param root 待解析的 JSON 根对象。
 * @param out 输出的动作事实报告。
 * @return 校验成功返回 Ok，否则返回契约错误。
 */
Status ParseReminderActionStatusReport(const JsonValue& root, ReminderActionStatusReport& out);

}  // namespace voicelife::contracts::im
