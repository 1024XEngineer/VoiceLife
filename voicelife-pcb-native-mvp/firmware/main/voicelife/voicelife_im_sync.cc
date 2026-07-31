#include "voicelife_im_sync.h"

#include "voicelife_service.h"

#include <cJSON.h>

#include <cctype>
#include <cmath>
#include <utility>

namespace voicelife {
namespace {

constexpr size_t kMaxJsonBody = 64 * 1024;

std::string JsonString(const cJSON* object, const char* key) {
    const cJSON* value = object == nullptr ? nullptr : cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(value) && value->valuestring != nullptr ? value->valuestring : std::string{};
}

bool JsonBool(const cJSON* object, const char* key, bool* present = nullptr) {
    const cJSON* value = object == nullptr ? nullptr : cJSON_GetObjectItemCaseSensitive(object, key);
    if (present != nullptr) *present = cJSON_IsBool(value);
    return cJSON_IsTrue(value);
}

std::string PrintAndDelete(cJSON* object) {
    if (object == nullptr) return {};
    char* text = cJSON_PrintUnformatted(object);
    std::string result = text == nullptr ? std::string{} : std::string(text);
    if (text != nullptr) cJSON_free(text);
    cJSON_Delete(object);
    return result;
}

std::string UrlEncodeSegment(const std::string& value) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string encoded;
    for (unsigned char byte : value) {
        if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
            (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' || byte == '.' || byte == '~') {
            encoded.push_back(static_cast<char>(byte));
        } else {
            encoded.push_back('%');
            encoded.push_back(kHex[byte >> 4]);
            encoded.push_back(kHex[byte & 0x0f]);
        }
    }
    return encoded;
}

}  // namespace

VoiceLifeImSync::VoiceLifeImSync(VoiceLifeService* service, ImTransport* transport, Config config)
    : service_(service), transport_(transport), config_(std::move(config)) {}

void VoiceLifeImSync::Fail(const std::string& message) {
    last_error_ = message;
    ++consecutive_failures_;
}

bool VoiceLifeImSync::ParseOkResponse(const ImHttpResponse& response, bool* ok,
                                      std::string* message) const {
    if (!response.transport_ok) {
        if (message != nullptr) *message = response.error.empty() ? "HTTP transport failed" : response.error;
        return false;
    }
    if (response.status_code < 200 || response.status_code >= 300) {
        if (message != nullptr) *message = "HTTP status " + std::to_string(response.status_code);
        return false;
    }
    if (response.body.size() > kMaxJsonBody) {
        if (message != nullptr) *message = "Gateway response too large";
        return false;
    }
    cJSON* root = cJSON_Parse(response.body.c_str());
    if (root == nullptr || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        if (message != nullptr) *message = "Gateway response is not JSON";
        return false;
    }
    bool present = false;
    const bool value = JsonBool(root, "ok", &present);
    if (!present) {
        cJSON_Delete(root);
        if (message != nullptr) *message = "Gateway response has no ok field";
        return false;
    }
    if (ok != nullptr) *ok = value;
    if (message != nullptr) {
        *message = JsonString(root, "error");
        if (message->empty()) *message = JsonString(root, "message");
    }
    cJSON_Delete(root);
    return true;
}

bool VoiceLifeImSync::ReportDue(const std::string& payload) {
    cJSON* input = cJSON_Parse(payload.c_str());
    if (input == nullptr || !cJSON_IsObject(input)) {
        cJSON_Delete(input);
        Fail("invalid due payload");
        return false;
    }
    const std::string reminder_id = JsonString(input, "reminderId");
    const std::string due_at = JsonString(input, "dueAt");
    if (reminder_id.empty() || due_at.empty()) {
        cJSON_Delete(input);
        Fail("due payload missing reminder identity");
        return false;
    }
    cJSON_Delete(input);

    const ImHttpResponse response = transport_->Request("POST", "/api/pcb/reminders/due", payload);
    bool ok = false;
    std::string message;
    if (!ParseOkResponse(response, &ok, &message) || !ok) {
        Fail(message.empty() ? "Gateway rejected due notification" : message);
        return false;
    }
    if (!service_->MarkImNotificationReported(reminder_id, due_at)) {
        Fail("failed to persist due notification receipt");
        return false;
    }
    return true;
}

bool VoiceLifeImSync::AckAction(const std::string& action_id, const std::string& result_json,
                                bool ok, const std::string& error) {
    cJSON* body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "deviceId", config_.device_id.c_str());
    cJSON_AddBoolToObject(body, "ok", ok);
    cJSON* result = cJSON_Parse(result_json.c_str());
    if (result != nullptr && cJSON_IsObject(result)) {
        cJSON_AddItemToObject(body, "result", result);
    } else {
        cJSON_Delete(result);
        cJSON_AddNullToObject(body, "result");
    }
    if (!error.empty()) cJSON_AddStringToObject(body, "error", error.c_str());
    const std::string body_text = PrintAndDelete(body);
    const ImHttpResponse response = transport_->Request(
        "POST", "/api/pcb/actions/" + UrlEncodeSegment(action_id) + "/ack", body_text);
    bool gateway_ok = false;
    std::string message;
    if (!ParseOkResponse(response, &gateway_ok, &message)) {
        Fail(message.empty() ? "Gateway action ACK failed" : message);
        return false;
    }
    // A gateway `ok:false` is a valid business ACK for a local action error;
    // the gateway itself decides whether to retry it. Only transport/protocol
    // failures make this poll fail.
    (void)gateway_ok;
    return true;
}

bool VoiceLifeImSync::PullAndApplyAction() {
    const ImHttpResponse response = transport_->Request(
        "GET", "/api/pcb/devices/" + UrlEncodeSegment(config_.device_id) + "/actions/next", {});
    if (!response.transport_ok || response.status_code < 200 || response.status_code >= 300 ||
        response.body.size() > kMaxJsonBody) {
        bool ignored = false;
        std::string message;
        ParseOkResponse(response, &ignored, &message);
        Fail(message.empty() ? "failed to pull Gateway action" : message);
        return false;
    }
    cJSON* root = cJSON_Parse(response.body.c_str());
    if (root == nullptr || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        Fail("Gateway action response is not JSON");
        return false;
    }
    bool ok_present = false;
    const bool gateway_ok = JsonBool(root, "ok", &ok_present);
    if (!ok_present || !gateway_ok) {
        cJSON_Delete(root);
        Fail("Gateway rejected action pull");
        return false;
    }
    const cJSON* action = cJSON_GetObjectItemCaseSensitive(root, "action");
    if (action == nullptr || cJSON_IsNull(action)) {
        cJSON_Delete(root);
        return true;
    }
    if (!cJSON_IsObject(action)) {
        cJSON_Delete(root);
        Fail("Gateway action is not an object");
        return false;
    }
    const std::string action_id = JsonString(action, "id");
    const std::string type = JsonString(action, "type");
    const std::string reminder_id = JsonString(action, "reminderId");
    const cJSON* minutes_value = cJSON_GetObjectItemCaseSensitive(action, "minutes");
    int minutes = 0;
    if (cJSON_IsNumber(minutes_value) && std::floor(minutes_value->valuedouble) == minutes_value->valuedouble) {
        minutes = minutes_value->valueint;
    }
    cJSON_Delete(root);
    if (action_id.empty() || type.empty() || reminder_id.empty()) {
        Fail("Gateway action missing identity");
        return false;
    }
    const std::string result_json = service_->ApplyImAction(action_id, type, reminder_id, minutes);
    cJSON* local_result = cJSON_Parse(result_json.c_str());
    bool local_ok = false;
    std::string local_error = "invalid local action result";
    if (local_result != nullptr && cJSON_IsObject(local_result)) {
        bool present = false;
        local_ok = JsonBool(local_result, "ok", &present);
        if (!present) local_ok = false;
        local_error = JsonString(local_result, "message");
    }
    cJSON_Delete(local_result);
    return AckAction(action_id, result_json, local_ok, local_ok ? std::string{} : local_error);
}

bool VoiceLifeImSync::PollOnce() {
    last_error_.clear();
    if (service_ == nullptr || transport_ == nullptr || config_.device_id.empty()) {
        Fail("IM sync is not configured");
        return false;
    }
    bool all_ok = true;
    const std::vector<std::string> pending = service_->CollectPendingImNotifications(config_.device_id);
    for (const auto& payload : pending) {
        if (!ReportDue(payload)) all_ok = false;
    }
    if (!PullAndApplyAction()) all_ok = false;
    if (all_ok) {
        consecutive_failures_ = 0;
        last_error_.clear();
    }
    return all_ok;
}

}  // namespace voicelife
