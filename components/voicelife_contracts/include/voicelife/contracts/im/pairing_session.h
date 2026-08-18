#pragma once

#include <optional>
#include <string>
#include <vector>

#include "voicelife/contracts/json.h"

namespace voicelife::contracts::im {

/// 设备创建短期配对会话的公开请求。
struct CreatePairingSessionRequest {
    std::optional<std::string> userId;
    std::string deviceId;
    std::optional<std::vector<std::string>> allowedPlatforms;
    std::optional<int> expiresInMinutes;
};

/// 设备可见的配对会话状态；不包含展示码 hash 或外部 IM 身份。
struct PairingSessionStatus {
    std::string id;
    std::optional<std::string> userId;
    std::string deviceId;
    std::optional<std::vector<std::string>> allowedPlatforms;
    std::string status;
    std::string expiresAt;
    std::string createdAt;
    std::optional<std::string> confirmedAt;
};

/// Gateway 创建配对会话后返回的公开状态与六位展示码。
struct CreatedPairingSession {
    PairingSessionStatus session;
    std::string displayCode;
};

/**
 * @brief 解析并校验设备创建配对会话的请求。
 * @param root 已解析的 JSON 请求对象。
 * @param out 解析成功后的请求。
 * @return 契约非法时返回 kInvalidArgument。
 */
Status ParseCreatePairingSessionRequest(const JsonValue& root, CreatePairingSessionRequest& out);

/**
 * @brief 解析并校验 Gateway 返回的公开配对状态。
 * @param root 已解析的 JSON 状态对象。
 * @param out 解析成功后的公开状态。
 * @return 字段非法或包含 displayCodeHash 时返回 kInvalidArgument。
 */
Status ParsePairingSessionStatus(const JsonValue& root, PairingSessionStatus& out);

/**
 * @brief 解析并校验 Gateway 创建配对会话的响应。
 * @param root 已解析的 JSON 响应对象。
 * @param out 解析成功后的会话与六位码。
 * @return 契约非法、会话非 pending 或展示码格式错误时返回 kInvalidArgument。
 */
Status ParseCreatedPairingSession(const JsonValue& root, CreatedPairingSession& out);

}  // namespace voicelife::contracts::im
