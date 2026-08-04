#pragma once

#include <optional>
#include <string>
#include <vector>

#include "im_contracts.h"
#include "voicelife/contracts/json.h"

namespace voicelife::contracts::im {

/// 通知内容携带的一个可执行动作（type 为 acknowledge 或 snooze）。
struct NotificationAction {
    std::string kind;            ///< 固定为 "command"。
    std::string type;            ///< "acknowledge" 或 "snooze"。
    std::string label;           ///< 面向用户的动作文案，UTF-8。
    std::optional<int> minutes;  ///< snooze 动作必需的推迟分钟数。
};

/// 通知收件人：绑定到设备与用户的稳定身份。
struct NotificationRecipient {
    std::string userId;
    std::string deviceId;
};

/// 通知展示内容。
struct NotificationContent {
    std::string title;
    std::optional<std::string> body;
};

/// 设备上报的强/弱提醒通知意图，字段语义与 TypeScript NotificationIntent 一致。
struct NotificationIntent {
    std::string schemaVersion;
    std::string businessEventId;
    std::string correlationId;
    std::string kind;  ///< 固定为 "reminder_due"。
    NotificationRecipient recipient;
    std::string scheduleId;
    std::string taskId;
    std::string instanceId;
    std::string reminderTriggerId;
    std::string reminderType;  ///< "weak" 或 "strong"。
    NotificationContent content;
    std::vector<NotificationAction> actions;
    std::string plannedAt;
    std::string triggerAt;
    std::string occurredAt;
};

/**
 * @brief 解析并校验 NotificationIntent。
 * @param root 已解析的 JSON 对象。
 * @param out 解析成功后的意图。
 * @return 契约非法时返回 kInvalidArgument，语义与 TypeScript parseNotificationIntent 一致。
 */
Status ParseNotificationIntent(const JsonValue& root, NotificationIntent& out);

}  // namespace voicelife::contracts::im
