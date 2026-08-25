#include "voicelife/linx/linx_ota.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>

#include "cJSON.h"

namespace voicelife::linx {
namespace {

constexpr std::string_view kOfficialLinxWsPrefix = "ws://xrobo-io.qiniuapi.com/";

struct JsonDeleter {
    void operator()(cJSON* value) const {
        if (value != nullptr) cJSON_Delete(value);
    }
};
using JsonPtr = std::unique_ptr<cJSON, JsonDeleter>;

Result<std::string> PrintJson(cJSON* value) {
    char* serialized = cJSON_PrintUnformatted(value);
    if (serialized == nullptr) return Result<std::string>::Failure(ErrorCode::kInternal, "Linx OTA JSON 序列化失败");
    std::string result(serialized);
    cJSON_free(serialized);
    return Result<std::string>::Success(std::move(result));
}

bool ValidText(const std::string& value) {
    if (value.empty()) return false;
    for (const unsigned char character : value) {
        if (character < 0x20U || character == 0x7fU) return false;
    }
    return true;
}

Status RequireString(const cJSON* object, const char* key, std::string& output) {
    const cJSON* value = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsString(value) || cJSON_GetStringValue(value)[0] == '\0') {
        return Status::Error(ErrorCode::kInvalidArgument, std::string("Linx OTA 缺少字符串字段: ") + key);
    }
    output = cJSON_GetStringValue(value);
    return Status::Ok();
}

Status OptionalString(const cJSON* object, const char* key, std::optional<std::string>& output) {
    const cJSON* value = cJSON_GetObjectItemCaseSensitive(object, key);
    if (value == nullptr) return Status::Ok();
    if (!cJSON_IsString(value) || cJSON_GetStringValue(value)[0] == '\0') {
        return Status::Error(ErrorCode::kInvalidArgument, std::string("Linx OTA 字符串字段无效: ") + key);
    }
    output = cJSON_GetStringValue(value);
    return Status::Ok();
}

Status OptionalInteger(const cJSON* object, const char* key, int32_t& output, bool& present) {
    const cJSON* value = cJSON_GetObjectItemCaseSensitive(object, key);
    if (value == nullptr) return Status::Ok();
    if (!cJSON_IsNumber(value) || !std::isfinite(value->valuedouble) ||
        value->valuedouble < std::numeric_limits<int32_t>::min() ||
        value->valuedouble > std::numeric_limits<int32_t>::max() ||
        std::floor(value->valuedouble) != value->valuedouble) {
        return Status::Error(ErrorCode::kInvalidArgument, std::string("Linx OTA 数字字段无效: ") + key);
    }
    output = value->valueint;
    present = true;
    return Status::Ok();
}

Status ParseActivation(const cJSON* root, std::optional<LinxActivation>& output) {
    const cJSON* value = cJSON_GetObjectItemCaseSensitive(root, "activation");
    if (value == nullptr) return Status::Ok();
    if (!cJSON_IsObject(value)) return Status::Error(ErrorCode::kInvalidArgument, "Linx OTA activation 必须是对象");
    LinxActivation activation;
    if (const Status status = RequireString(value, "code", activation.code); !status.ok()) return status;
    if (const Status status = RequireString(value, "message", activation.message); !status.ok()) return status;
    if (const Status status = OptionalString(value, "challenge", activation.challenge); !status.ok()) return status;
    output = std::move(activation);
    return Status::Ok();
}

Status ParseWebSocket(const cJSON* root, std::optional<LinxOtaWebSocket>& output) {
    const cJSON* value = cJSON_GetObjectItemCaseSensitive(root, "websocket");
    if (value == nullptr) return Status::Ok();
    if (!cJSON_IsObject(value)) return Status::Error(ErrorCode::kInvalidArgument, "Linx OTA websocket 必须是对象");
    LinxOtaWebSocket websocket;
    if (const Status status = RequireString(value, "url", websocket.url); !status.ok()) return status;
    if (const Status status = OptionalString(value, "token", websocket.token); !status.ok()) return status;
    output = std::move(websocket);
    return Status::Ok();
}

Status ParseServerTime(const cJSON* root, std::optional<LinxServerTime>& output) {
    const cJSON* value = cJSON_GetObjectItemCaseSensitive(root, "server_time");
    if (value == nullptr) return Status::Ok();
    if (!cJSON_IsObject(value)) return Status::Error(ErrorCode::kInvalidArgument, "Linx OTA server_time 必须是对象");
    const cJSON* timestamp = cJSON_GetObjectItemCaseSensitive(value, "timestamp");
    if (!cJSON_IsNumber(timestamp) || !std::isfinite(timestamp->valuedouble) || timestamp->valuedouble < 0 ||
        timestamp->valuedouble > static_cast<double>(std::numeric_limits<uint64_t>::max()) ||
        std::floor(timestamp->valuedouble) != timestamp->valuedouble) {
        return Status::Error(ErrorCode::kInvalidArgument, "Linx OTA server_time.timestamp 无效");
    }
    LinxServerTime server_time;
    server_time.timestamp_ms = static_cast<uint64_t>(timestamp->valuedouble);
    if (const Status status = OptionalString(value, "timezone", server_time.timezone); !status.ok()) return status;
    if (!server_time.timezone.has_value()) {
        if (const Status status = OptionalString(value, "timeZone", server_time.timezone); !status.ok()) return status;
    }
    int32_t offset = 0;
    bool offset_present = false;
    if (const Status status = OptionalInteger(value, "timezone_offset", offset, offset_present); !status.ok())
        return status;
    if (offset_present) server_time.timezone_offset_minutes = offset;
    output = std::move(server_time);
    return Status::Ok();
}

Status ParseFirmware(const cJSON* root, std::optional<LinxFirmwareUpdate>& output) {
    const cJSON* value = cJSON_GetObjectItemCaseSensitive(root, "firmware");
    if (value == nullptr) return Status::Ok();
    if (!cJSON_IsObject(value)) return Status::Error(ErrorCode::kInvalidArgument, "Linx OTA firmware 必须是对象");
    LinxFirmwareUpdate firmware;
    if (const Status status = RequireString(value, "version", firmware.version); !status.ok()) return status;
    if (const Status status = OptionalString(value, "url", firmware.url); !status.ok()) return status;
    output = std::move(firmware);
    return Status::Ok();
}

Result<std::string> SecureWebSocketUrl(std::string_view url) {
    if (url.rfind("wss://", 0) == 0) return Result<std::string>::Success(std::string(url));
    if (url.rfind(kOfficialLinxWsPrefix, 0) == 0) {
        return Result<std::string>::Success("wss://" + std::string(url.substr(5)));
    }
    return Result<std::string>::Failure(ErrorCode::kInvalidArgument, "Linx OTA WebSocket 地址不是受信任的 TLS 地址");
}

}  // namespace

Result<LinxOtaHttpRequest> BuildLinxOtaRequest(const LinxOtaDeviceInfo& device) {
    if ((device.activation_version != "1" && device.activation_version != "2") || !ValidText(device.device_id) ||
        !ValidText(device.client_id) || !ValidText(device.user_agent) || !ValidText(device.application_name) ||
        !ValidText(device.application_version) || !ValidText(device.application_elf_sha256) ||
        !ValidText(device.chip_model_name) || !ValidText(device.board_type) || !ValidText(device.board_name) ||
        (device.serial_number.has_value() && !ValidText(*device.serial_number)) ||
        (device.accept_language.has_value() && !ValidText(*device.accept_language)) || device.flash_size_bytes == 0 ||
        device.partition_table_json.empty()) {
        return Result<LinxOtaHttpRequest>::Failure(ErrorCode::kInvalidArgument, "Linx OTA 请求设备信息不完整");
    }
    JsonPtr partitions(cJSON_ParseWithLength(device.partition_table_json.data(), device.partition_table_json.size()));
    if (!cJSON_IsArray(partitions.get())) {
        return Result<LinxOtaHttpRequest>::Failure(ErrorCode::kInvalidArgument, "Linx OTA 分区表必须是 JSON 数组");
    }
    JsonPtr root(cJSON_CreateObject());
    cJSON_AddNumberToObject(root.get(), "version", 0);
    cJSON_AddStringToObject(root.get(), "uuid", device.client_id.c_str());
    cJSON* application = cJSON_AddObjectToObject(root.get(), "application");
    cJSON_AddStringToObject(application, "name", device.application_name.c_str());
    cJSON_AddStringToObject(application, "version", device.application_version.c_str());
    cJSON_AddStringToObject(application, "elf_sha256", device.application_elf_sha256.c_str());
    cJSON* board = cJSON_AddObjectToObject(root.get(), "board");
    cJSON_AddStringToObject(board, "type", device.board_type.c_str());
    cJSON_AddStringToObject(board, "name", device.board_name.c_str());
    cJSON_AddStringToObject(board, "ssid", device.wifi_ssid.c_str());
    cJSON_AddNumberToObject(board, "rssi", device.wifi_rssi);
    cJSON_AddNumberToObject(root.get(), "flash_size", device.flash_size_bytes);
    if (device.psram_size_bytes != 0) cJSON_AddNumberToObject(root.get(), "psram_size", device.psram_size_bytes);
    cJSON_AddStringToObject(root.get(), "mac_address", device.device_id.c_str());
    cJSON_AddStringToObject(root.get(), "chip_model_name", device.chip_model_name.c_str());
    cJSON_AddItemToObject(root.get(), "partition_table", partitions.release());
    auto body = PrintJson(root.get());
    if (!body.ok() || !body.value.has_value())
        return Result<LinxOtaHttpRequest>::Failure(body.status.code, body.status.message);
    LinxOtaHttpRequest request;
    request.headers = {{"Activation-Version", device.activation_version},
                       {"Device-Id", device.device_id},
                       {"Client-Id", device.client_id},
                       {"User-Agent", device.user_agent},
                       {"Content-Type", "application/json"}};
    if (device.serial_number.has_value()) request.headers.push_back({"Serial-Number", *device.serial_number});
    if (device.accept_language.has_value()) request.headers.push_back({"Accept-Language", *device.accept_language});
    request.body = std::move(*body.value);
    return Result<LinxOtaHttpRequest>::Success(std::move(request));
}

Result<LinxOtaResponse> ParseLinxOtaResponse(std::string_view body) {
    JsonPtr root(cJSON_ParseWithLength(body.data(), body.size()));
    if (!cJSON_IsObject(root.get())) {
        return Result<LinxOtaResponse>::Failure(ErrorCode::kInvalidArgument, "Linx OTA 响应必须是 JSON 对象");
    }
    const cJSON* error = cJSON_GetObjectItemCaseSensitive(root.get(), "error");
    if (cJSON_IsString(error) && cJSON_GetStringValue(error)[0] != '\0') {
        return Result<LinxOtaResponse>::Failure(ErrorCode::kUnavailable, "Linx OTA 服务拒绝请求");
    }
    LinxOtaResponse response;
    if (const Status status = ParseActivation(root.get(), response.activation); !status.ok())
        return Result<LinxOtaResponse>::Failure(status.code, status.message);
    if (const Status status = ParseWebSocket(root.get(), response.websocket); !status.ok())
        return Result<LinxOtaResponse>::Failure(status.code, status.message);
    if (const Status status = ParseServerTime(root.get(), response.server_time); !status.ok())
        return Result<LinxOtaResponse>::Failure(status.code, status.message);
    if (const Status status = ParseFirmware(root.get(), response.firmware); !status.ok())
        return Result<LinxOtaResponse>::Failure(status.code, status.message);
    return Result<LinxOtaResponse>::Success(std::move(response));
}

Result<LinxConnectionConfig> BuildLinxConnectionConfig(const LinxOtaResponse& response, std::string_view device_id,
                                                       std::string_view client_id, std::string_view token_reference) {
    if (!response.websocket.has_value() || !response.websocket->token.has_value() ||
        !ValidText(*response.websocket->token) || device_id.empty() || client_id.empty() ||
        token_reference.rfind("nvs://", 0) != 0) {
        return Result<LinxConnectionConfig>::Failure(ErrorCode::kInvalidArgument,
                                                     "Linx OTA 未提供可安全保存的 WSS 连接配置");
    }
    auto websocket_url = SecureWebSocketUrl(response.websocket->url);
    if (!websocket_url.ok() || !websocket_url.value.has_value()) {
        return Result<LinxConnectionConfig>::Failure(websocket_url.status.code, websocket_url.status.message);
    }
    LinxConnectionConfig config{.websocket_url = std::move(*websocket_url.value),
                                .token_ref = std::string(token_reference),
                                .device_id = std::string(device_id),
                                .client_id = std::string(client_id),
                                .agent_id = std::nullopt,
                                .preferred_audio = std::nullopt};
    if (!config.valid()) {
        return Result<LinxConnectionConfig>::Failure(ErrorCode::kInvalidArgument, "Linx OTA 连接配置无效");
    }
    return Result<LinxConnectionConfig>::Success(std::move(config));
}

}  // namespace voicelife::linx
