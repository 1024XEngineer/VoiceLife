#include "voicelife/contracts/im/pairing_session.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "contract_parsing.h"

namespace voicelife::contracts::im {
namespace {

using detail::OptionalIsoDateTime;
using detail::OptionalString;
using detail::Reject;
using detail::RequireEnum;
using detail::RequireIsoDateTime;
using detail::RequireString;

constexpr int kMinimumPairingMinutes = 1;
constexpr int kMaximumPairingMinutes = 10;

bool IsAllowedPlatform(std::string_view platform) {
    return platform == "wechat_official" || platform == "wecom_aibot" || platform == "feishu" || platform == "dingtalk";
}

Status ParseOptionalPlatforms(const JsonValue& root, std::optional<std::vector<std::string>>& out) {
    const JsonValue* value = root.Get("allowedPlatforms");
    if (value == nullptr) return Status::Ok();
    if (!value->IsArray()) return Reject("allowedPlatforms 必须是数组");
    std::vector<std::string> platforms;
    platforms.reserve(value->array.size());
    for (const JsonValue& platform : value->array) {
        if (!platform.IsString() || !IsAllowedPlatform(platform.string)) {
            return Reject("allowedPlatforms 含未知平台");
        }
        platforms.push_back(platform.string);
    }
    out = std::move(platforms);
    return Status::Ok();
}

Status ParseOptionalPairingMinutes(const JsonValue& root, std::optional<int>& out) {
    const JsonValue* value = root.Get("expiresInMinutes");
    if (value == nullptr) return Status::Ok();
    if (value->kind != JsonValue::Kind::kNumber || !std::isfinite(value->number) ||
        value->number != std::floor(value->number) || value->number < kMinimumPairingMinutes ||
        value->number > kMaximumPairingMinutes) {
        return Reject("expiresInMinutes 必须是 1 到 10 的整数");
    }
    out = static_cast<int>(value->number);
    return Status::Ok();
}

Status ParsePairingSessionStatusValue(const JsonValue& root, PairingSessionStatus& out) {
    if (!root.IsObject()) return Reject("PairingSessionStatus 必须是对象");
    if (root.Get("displayCodeHash") != nullptr) return Reject("设备响应不得包含 displayCodeHash");
    if (const Status status = RequireString(root, "id", out.id); !status.ok()) return status;
    if (const Status status = OptionalString(root, "userId", out.userId); !status.ok()) return status;
    if (const Status status = RequireString(root, "deviceId", out.deviceId); !status.ok()) return status;
    if (const Status status = ParseOptionalPlatforms(root, out.allowedPlatforms); !status.ok()) return status;
    if (const Status status = RequireEnum(root, "status", {"pending", "confirmed", "expired", "cancelled"}, out.status);
        !status.ok()) {
        return status;
    }
    if (const Status status = RequireIsoDateTime(root, "expiresAt", out.expiresAt); !status.ok()) return status;
    if (const Status status = RequireIsoDateTime(root, "createdAt", out.createdAt); !status.ok()) return status;
    if (const Status status = OptionalIsoDateTime(root, "confirmedAt", out.confirmedAt); !status.ok()) return status;
    if ((out.status == "confirmed") != out.confirmedAt.has_value()) {
        return Reject("confirmed 状态与 confirmedAt 必须一致");
    }
    return Status::Ok();
}

bool IsSixDigitCode(const std::string& value) {
    return value.size() == 6 && std::all_of(value.begin(), value.end(), [](unsigned char character) {
               return character >= '0' && character <= '9';
           });
}

}  // namespace

Status ParseCreatePairingSessionRequest(const JsonValue& root, CreatePairingSessionRequest& out) {
    if (!root.IsObject()) return Reject("CreatePairingSessionRequest 必须是对象");
    CreatePairingSessionRequest parsed;
    if (const Status status = OptionalString(root, "userId", parsed.userId); !status.ok()) return status;
    if (const Status status = RequireString(root, "deviceId", parsed.deviceId); !status.ok()) return status;
    if (const Status status = ParseOptionalPlatforms(root, parsed.allowedPlatforms); !status.ok()) return status;
    if (const Status status = ParseOptionalPairingMinutes(root, parsed.expiresInMinutes); !status.ok()) return status;
    out = std::move(parsed);
    return Status::Ok();
}

Status ParsePairingSessionStatus(const JsonValue& root, PairingSessionStatus& out) {
    PairingSessionStatus parsed;
    if (const Status status = ParsePairingSessionStatusValue(root, parsed); !status.ok()) return status;
    out = std::move(parsed);
    return Status::Ok();
}

Status ParseCreatedPairingSession(const JsonValue& root, CreatedPairingSession& out) {
    if (!root.IsObject()) return Reject("CreatedPairingSession 必须是对象");
    const JsonValue* session = root.Get("session");
    if (session == nullptr) return Reject("创建响应缺少 session");
    CreatedPairingSession parsed;
    if (const Status status = ParsePairingSessionStatusValue(*session, parsed.session); !status.ok()) return status;
    if (parsed.session.status != "pending") return Reject("新建配对会话必须是 pending");
    if (const Status status = RequireString(root, "displayCode", parsed.displayCode); !status.ok()) return status;
    if (!IsSixDigitCode(parsed.displayCode)) return Reject("displayCode 必须是六位数字");
    out = std::move(parsed);
    return Status::Ok();
}

}  // namespace voicelife::contracts::im
