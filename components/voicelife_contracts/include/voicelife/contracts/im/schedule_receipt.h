#pragma once

#include <optional>
#include <string>

#include "im_contracts.h"
#include "voicelife/contracts/json.h"

namespace voicelife::contracts::im {

/// 设备侧日程操作回执，字段语义与 TypeScript ScheduleReceiptIntent 一致。
struct ScheduleReceiptIntent {
    std::string schemaVersion;
    std::string eventId;
    std::string correlationId;
    std::optional<std::string> userId;  ///< 可选，缺失时省略。
    std::string deviceId;
    std::string operationType;  ///< created | updated | cancelled | undone。
    std::string scheduleId;
    std::string result;  ///< succeeded | failed。
    std::string summary;
    std::string occurredAt;
};

/**
 * @brief 解析并校验 ScheduleReceiptIntent。
 * @param root 已解析的 JSON 对象。
 * @param out 解析成功后的回执意图。
 * @return 契约非法时返回 kInvalidArgument，语义与 TypeScript parseScheduleReceiptIntent 一致。
 */
Status ParseScheduleReceiptIntent(const JsonValue& root, ScheduleReceiptIntent& out);

}  // namespace voicelife::contracts::im
