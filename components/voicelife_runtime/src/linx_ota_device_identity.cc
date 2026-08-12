#include "linx_ota_device_identity.h"

#ifdef ESP_PLATFORM

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>

#include "esp_mac.h"
#include "esp_random.h"
#include "linx_ota_bootstrap.h"
#include "nvs.h"

namespace voicelife::runtime {
namespace {

constexpr char kLinxNamespace[] = "linx";
constexpr char kClientIdKey[] = "client_id";

Result<std::string> ReadNvsString(nvs_handle_t handle, const char* key) {
    size_t size = 0;
    if (const esp_err_t error = nvs_get_str(handle, key, nullptr, &size); error != ESP_OK || size <= 1) {
        return Result<std::string>::Failure(ErrorCode::kNotFound, "Linx NVS 字段不可用");
    }
    std::string value(size, '\0');
    if (const esp_err_t error = nvs_get_str(handle, key, value.data(), &size); error != ESP_OK) {
        return Result<std::string>::Failure(ErrorCode::kUnavailable, "读取 Linx NVS 字段失败");
    }
    value.resize(size - 1);
    return Result<std::string>::Success(std::move(value));
}

std::string NewUuidV4() {
    std::array<uint8_t, 16> bytes{};
    for (size_t index = 0; index < bytes.size(); index += sizeof(uint32_t)) {
        const uint32_t random = esp_random();
        const size_t count = std::min(sizeof(random), bytes.size() - index);
        std::memcpy(bytes.data() + index, &random, count);
    }
    bytes[6] = static_cast<uint8_t>((bytes[6] & 0x0fU) | 0x40U);
    bytes[8] = static_cast<uint8_t>((bytes[8] & 0x3fU) | 0x80U);
    char result[37]{};
    std::snprintf(result, sizeof(result), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                  bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7], bytes[8], bytes[9],
                  bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
    return result;
}

}  // namespace

Result<std::string> LoadOrCreateLinxClientId() {
    nvs_handle_t handle = 0;
    if (const esp_err_t error =
            nvs_open_from_partition(LinxSecretPartitionLabel(), kLinxNamespace, NVS_READWRITE, &handle);
        error != ESP_OK) {
        return Result<std::string>::Failure(ErrorCode::kUnavailable, "打开 Linx NVS 失败");
    }
    auto existing = ReadNvsString(handle, kClientIdKey);
    if (existing.ok() && existing.value.has_value()) {
        nvs_close(handle);
        return existing;
    }
    const std::string client_id = NewUuidV4();
    const esp_err_t write_status = nvs_set_str(handle, kClientIdKey, client_id.c_str());
    const esp_err_t commit_status = write_status == ESP_OK ? nvs_commit(handle) : write_status;
    nvs_close(handle);
    if (commit_status != ESP_OK) {
        return Result<std::string>::Failure(ErrorCode::kUnavailable, "保存 Linx Client-Id 失败");
    }
    return Result<std::string>::Success(client_id);
}

std::string ReadLinxOtaDeviceId() {
    uint8_t mac[6]{};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) return {};
    char value[18]{};
    std::snprintf(value, sizeof(value), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4],
                  mac[5]);
    return value;
}

std::string EncodeLinxOtaHexDigest(const uint8_t* digest, size_t size) {
    std::string result;
    result.reserve(size * 2);
    constexpr char kHex[] = "0123456789abcdef";
    for (size_t index = 0; index < size; ++index) {
        result.push_back(kHex[digest[index] >> 4U]);
        result.push_back(kHex[digest[index] & 0x0fU]);
    }
    return result;
}

}  // namespace voicelife::runtime

#endif
