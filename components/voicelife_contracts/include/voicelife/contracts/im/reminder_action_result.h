#pragma once

#include <optional>
#include <string>

#include "im_contracts.h"
#include "voicelife/contracts/json.h"

namespace voicelife::contracts::im {

/// 设备侧动作执行结果回传，字段语义与 TypeScript ReminderActionResult 一致。
struct ReminderActionResult {
    std::string schemaVersion;
    std::string operationId;
    std::string reminderTriggerId;
    std::string status;                        ///< succeeded | retryable_failed | failed | expired。
    std::optional<std::string> nextTriggerAt;  ///< 推迟等动作后的下一次触发时间。
    std::optional<std::string> errorCode;
    std::optional<JsonValue> details;
    std::string occurredAt;
};

/**
 * @brief 解析并校验 ReminderActionResult。
 * @param root 已解析的 JSON 对象。
 * @param out 解析成功后的动作结果。
 * @return 契约非法时返回 kInvalidArgument，语义与 TypeScript parseReminderActionResult 一致。
 */
Status ParseReminderActionResult(const JsonValue& root, ReminderActionResult& out);

}  // namespace voicelife::contracts::im
