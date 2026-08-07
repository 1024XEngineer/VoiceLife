#pragma once

#include <optional>
#include <string>
#include <vector>

#include "im_contracts.h"
#include "voicelife/contracts/json.h"

namespace voicelife::contracts::im {

/// 网关受理通知投递后返回的交付行。
struct NotificationDelivery {
    std::string deliveryId;
    std::string bindingId;
    std::string status;  ///< 固定为 "pending"。
};

/// 强提醒的动作流窗口：设备在该窗口内建立临时 SSE。
struct NotificationActionStream {
    std::string reminderTriggerId;
    std::string expiresAt;
};

/// 设备上报通知意图后网关返回的受理结果。
struct NotificationSubmission {
    std::string businessEventId;
    std::string status;  ///< 固定为 "accepted"。
    std::vector<NotificationDelivery> deliveries;
    std::optional<NotificationActionStream> actionStream;
};

/**
 * @brief 解析并校验 NotificationSubmission。
 * @param root 已解析的 JSON 对象（通知上报的响应体）。
 * @param out 解析成功后的受理结果。
 * @return 契约非法时返回 kInvalidArgument，语义与 TypeScript NotificationSubmission 一致。
 */
Status ParseNotificationSubmission(const JsonValue& root, NotificationSubmission& out);

}  // namespace voicelife::contracts::im
