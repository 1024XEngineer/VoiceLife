#include "voicelife/im/im_pairing_client.h"

#include <cstdio>
#include <string>

#include "im_wire.h"
#include "voicelife/contracts/json.h"

namespace voicelife::im {
namespace {

constexpr const char* kPairingPath = "/v1/im/pairing-sessions";

void AppendJsonString(std::string& out, const std::string& value) {
    out.push_back('"');
    for (const unsigned char character : value) {
        switch (character) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (character < 0x20) {
                    char escaped[7];
                    std::snprintf(escaped, sizeof escaped, "\\u%04x", character);
                    out += escaped;
                } else {
                    out.push_back(static_cast<char>(character));
                }
        }
    }
    out.push_back('"');
}

std::string SerializeRequest(const PairingCreateOptions& options, const std::string& device_id) {
    std::string body = "{";
    if (options.user_id.has_value()) {
        body += "\"userId\":";
        AppendJsonString(body, *options.user_id);
        body.push_back(',');
    }
    body += "\"deviceId\":";
    AppendJsonString(body, device_id);
    body += ",\"allowedPlatforms\":[\"wechat_official\"],\"expiresInMinutes\":";
    body += std::to_string(options.expires_in_minutes);
    body.push_back('}');
    return body;
}

PairingClientStatus Classify(const ImHttpResponse& response, bool querying) {
    if (response.status == ImTransportStatus::kCredentialRejected) return PairingClientStatus::kCredentialRejected;
    if (response.status == ImTransportStatus::kNetworkFailure) return PairingClientStatus::kRetryable;
    if (response.status == ImTransportStatus::kInvalidConfig) return PairingClientStatus::kRejected;
    if (response.status == ImTransportStatus::kHttpError) {
        if (querying && response.status_code == 404) return PairingClientStatus::kNotFound;
        if (response.status_code == 408 || response.status_code == 429 || response.status_code >= 500) {
            return PairingClientStatus::kRetryable;
        }
        return PairingClientStatus::kRejected;
    }
    return PairingClientStatus::kSuccess;
}

}  // namespace

ImPairingClient::ImPairingClient(ImTransport& transport, ImCredentialProvider& credentials)
    : transport_(transport), credentials_(credentials) {}

PairingCreateResult ImPairingClient::Create(const PairingCreateOptions& options) {
    const std::string token = credentials_.DeviceToken();
    const std::string device_id = credentials_.DeviceId();
    if (token.empty() || device_id.empty()) {
        return {.status = PairingClientStatus::kCredentialRejected, .value = std::nullopt, .message = "设备凭据未配置"};
    }
    if (options.expires_in_minutes < 1 || options.expires_in_minutes > 10 ||
        (options.user_id.has_value() && options.user_id->empty())) {
        return {.status = PairingClientStatus::kRejected, .value = std::nullopt, .message = "配对选项非法"};
    }
    ImHttpRequest request{.path = kPairingPath,
                          .method = "POST",
                          .headers = {{"Content-Type", "application/json"}, {"Authorization", "Bearer " + token}},
                          .body = SerializeRequest(options, device_id)};
    const ImHttpResponse response = transport_.Post(request);
    const PairingClientStatus status = Classify(response, false);
    if (status != PairingClientStatus::kSuccess) {
        return {.status = status, .value = std::nullopt, .message = response.message};
    }

    JsonValue root;
    contracts::im::CreatedPairingSession value;
    if (!ParseJson(response.body, root).ok() || !contracts::im::ParseCreatedPairingSession(root, value).ok() ||
        value.session.deviceId != device_id) {
        return {.status = PairingClientStatus::kInvalidResponse, .value = std::nullopt, .message = "配对创建响应非法"};
    }
    return {.status = PairingClientStatus::kSuccess, .value = std::move(value), .message = {}};
}

PairingQueryResult ImPairingClient::Query(const std::string& pairing_session_id) {
    const std::string token = credentials_.DeviceToken();
    const std::string device_id = credentials_.DeviceId();
    if (token.empty() || device_id.empty()) {
        return {.status = PairingClientStatus::kCredentialRejected, .value = std::nullopt, .message = "设备凭据未配置"};
    }
    if (pairing_session_id.empty()) {
        return {.status = PairingClientStatus::kRejected, .value = std::nullopt, .message = "session id 为空"};
    }
    ImHttpRequest request{.path = std::string(kPairingPath) + "/" + EncodePathSegment(pairing_session_id),
                          .method = "GET",
                          .headers = {{"Authorization", "Bearer " + token}},
                          .body = {}};
    const ImHttpResponse response = transport_.Get(request);
    const PairingClientStatus status = Classify(response, true);
    if (status != PairingClientStatus::kSuccess) {
        return {.status = status, .value = std::nullopt, .message = response.message};
    }

    JsonValue root;
    contracts::im::PairingSessionStatus value;
    if (!ParseJson(response.body, root).ok() || !contracts::im::ParsePairingSessionStatus(root, value).ok() ||
        value.deviceId != device_id || value.id != pairing_session_id) {
        return {.status = PairingClientStatus::kInvalidResponse, .value = std::nullopt, .message = "配对查询响应非法"};
    }
    return {.status = PairingClientStatus::kSuccess, .value = std::move(value), .message = {}};
}

}  // namespace voicelife::im
