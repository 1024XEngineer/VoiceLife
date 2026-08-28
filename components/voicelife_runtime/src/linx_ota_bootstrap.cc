#include "linx_ota_bootstrap.h"

#include <fcntl.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_flash.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_partition.h"
#include "esp_psram.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "voicelife/linx/linx_ota.h"
#include "wifi_provisioning_esp.h"

namespace voicelife::runtime {
namespace {

constexpr char kLinxNamespace[] = "linx";
constexpr char kSecretPartition[] = "linx_secrets";
constexpr char kTokenKey[] = "token";
constexpr char kTokenReference[] = "nvs://linx/token";
constexpr char kClientIdKey[] = "client_id";
constexpr char kWifiNamespace[] = "wifi";
constexpr char kWifiSsidKey[] = "ssid";
constexpr char kWifiPasswordKey[] = "password";
constexpr char kWifiProvisioningRequestedKey[] = "reprov";
constexpr std::array<uint8_t, 4> kProvisionMagic = {'V', 'L', 'W', '1'};
constexpr size_t kMaxWifiSsidBytes = 32;
constexpr size_t kMaxWifiPasswordBytes = 64;
constexpr int kProvisionTimeoutMs = 45000;
constexpr int kRecoverySerialProvisionTimeoutMs = 15000;
constexpr size_t kMaxOtaResponseBytes = 16 * 1024;
constexpr EventBits_t kWifiConnectedBit = BIT0;
constexpr EventBits_t kWifiFailedBit = BIT1;
constexpr int kWifiConnectTimeoutMs = 15000;
constexpr int kWifiConnectWindowMs = 5 * 60 * 1000;
constexpr int kWifiRetryDelayMs = 1000;
constexpr int kOtaAttempts = 3;
constexpr int kOtaBootstrapAttempts = 5;
constexpr std::array<int, kOtaBootstrapAttempts - 1> kOtaRetryDelayMs = {1000, 2000, 4000, 8000};
constexpr char kTag[] = "VoiceLifeLinxOta";

struct WifiCredentials {
    std::string ssid;
    std::string password;
};

Status EspError(const char* operation, esp_err_t error);
Result<std::string> ReadNvsString(nvs_handle_t handle, const char* key);

esp_err_t OpenSecretNamespace(const char* name, nvs_open_mode_t mode, nvs_handle_t* handle) {
    return nvs_open_from_partition(kSecretPartition, name, mode, handle);
}

bool ReadConsoleBytes(uint8_t* destination, size_t size, int timeout_ms) {
    size_t received = 0;
    const int64_t deadline_us = esp_timer_get_time() + static_cast<int64_t>(timeout_ms) * 1000;
    // 非阻塞读取：fcntl(O_NONBLOCK)/poll 在 USB-Serial-JTAG console 的 stdin 下
    // 不可用；USB-JTAG vfs read 无数据时返回 0（非阻塞），UART 在超时后返回 0。
    // 因此直接 read + 轮询，兼容两种 console。
    while (received < size) {
        if (esp_timer_get_time() >= deadline_us) {
            return false;
        }
        const ssize_t count = read(STDIN_FILENO, destination + received, size - received);
        if (count > 0) {
            received += static_cast<size_t>(count);
            continue;
        }
        if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return received == size;
}

Status StoreWifiCredentials(std::string_view ssid, std::string_view password) {
#if !CONFIG_NVS_ENCRYPTION
    (void)ssid;
    (void)password;
    return Status::Error(ErrorCode::kUnavailable, "Wi-Fi 凭据存储需要启用 NVS encryption");
#else
    nvs_handle_t handle = 0;
    esp_err_t error = OpenSecretNamespace(kWifiNamespace, NVS_READWRITE, &handle);
    if (error != ESP_OK) return EspError("打开 Wi-Fi 加密凭据存储", error);
    error = nvs_set_str(handle, kWifiSsidKey, std::string(ssid).c_str());
    if (error == ESP_OK) error = nvs_set_str(handle, kWifiPasswordKey, std::string(password).c_str());
    if (error == ESP_OK) error = nvs_commit(handle);
    nvs_close(handle);
    return error == ESP_OK ? Status::Ok() : EspError("保存 Wi-Fi 加密凭据", error);
#endif
}

Status ProvisionWifiCredentialsFromConsole(int timeout_ms = kProvisionTimeoutMs) {
    ESP_LOGW(kTag, "LINX_WIFI_PROVISION_READY=1 timeout_ms=%d", timeout_ms);
    std::array<uint8_t, kProvisionMagic.size() + 2 + kMaxWifiSsidBytes + kMaxWifiPasswordBytes> request{};
    const size_t header_size = kProvisionMagic.size() + 2;
    if (!ReadConsoleBytes(request.data(), header_size, timeout_ms)) {
        ESP_LOGW(kTag, "LINX_WIFI_PROVISION_RESULT=header_timeout");
        return Status::Error(ErrorCode::kNotFound, "未收到物理串口 Wi-Fi 配网请求");
    }
    if (!std::equal(kProvisionMagic.begin(), kProvisionMagic.end(), request.begin())) {
        ESP_LOGW(kTag, "LINX_WIFI_PROVISION_RESULT=invalid_magic");
        return Status::Error(ErrorCode::kInvalidArgument, "物理串口 Wi-Fi 配网请求无效");
    }
    const size_t ssid_size = request[kProvisionMagic.size()];
    const size_t password_size = request[kProvisionMagic.size() + 1];
    if (ssid_size == 0 || ssid_size > kMaxWifiSsidBytes || password_size == 0 ||
        password_size > kMaxWifiPasswordBytes) {
        ESP_LOGW(kTag, "LINX_WIFI_PROVISION_RESULT=invalid_lengths ssid_bytes=%u password_bytes=%u",
                 static_cast<unsigned>(ssid_size), static_cast<unsigned>(password_size));
        return Status::Error(ErrorCode::kInvalidArgument, "物理串口 Wi-Fi 配网字段长度无效");
    }
    const size_t payload_size = ssid_size + password_size;
    if (!ReadConsoleBytes(request.data() + header_size, payload_size, timeout_ms)) {
        ESP_LOGW(kTag, "LINX_WIFI_PROVISION_RESULT=payload_timeout payload_bytes=%u",
                 static_cast<unsigned>(payload_size));
        return Status::Error(ErrorCode::kInvalidArgument, "物理串口 Wi-Fi 配网内容不完整");
    }
    const Status status = StoreWifiCredentials(
        std::string_view(reinterpret_cast<const char*>(request.data() + header_size), ssid_size),
        std::string_view(reinterpret_cast<const char*>(request.data() + header_size + ssid_size), password_size));
    std::fill(request.begin(), request.end(), 0);
    if (status.ok()) {
        ESP_LOGI(kTag, "LINX_WIFI_PROVISIONED=1");
    } else {
        ESP_LOGW(kTag, "LINX_WIFI_PROVISION_RESULT=store_failed code=%d", static_cast<int>(status.code));
    }
    return status;
}

void OnWifiEvent(void* argument, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    auto* events = static_cast<EventGroupHandle_t*>(argument);
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const auto* disconnected = static_cast<const wifi_event_sta_disconnected_t*>(event_data);
        ESP_LOGW(kTag, "WIFI_STA_DISCONNECTED reason=%d", disconnected == nullptr ? -1 : disconnected->reason);
        xEventGroupSetBits(*events, kWifiFailedBit);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(kTag, "WIFI_STA_GOT_IP=1");
        xEventGroupSetBits(*events, kWifiConnectedBit);
    }
}

Result<WifiCredentials> LoadWifiCredentials() {
#if !CONFIG_NVS_ENCRYPTION
    return Result<WifiCredentials>::Failure(ErrorCode::kUnavailable, "Wi-Fi 凭据存储需要启用 NVS encryption");
#else
    nvs_handle_t handle = 0;
    if (const esp_err_t error = OpenSecretNamespace(kWifiNamespace, NVS_READONLY, &handle); error != ESP_OK) {
        return Result<WifiCredentials>::Failure(ErrorCode::kNotFound, "未找到已预置的 Wi-Fi 凭据");
    }
    auto ssid = ReadNvsString(handle, kWifiSsidKey);
    auto password = ReadNvsString(handle, kWifiPasswordKey);
    nvs_close(handle);
    if (!ssid.ok() || !ssid.value.has_value() || !password.ok() || !password.value.has_value()) {
        return Result<WifiCredentials>::Failure(ErrorCode::kNotFound, "Wi-Fi 凭据不完整");
    }
    return Result<WifiCredentials>::Success(
        WifiCredentials{.ssid = std::move(*ssid.value), .password = std::move(*password.value)});
#endif
}

bool IsWifiProvisioningRequested() {
    nvs_handle_t handle = 0;
    if (OpenSecretNamespace(kWifiNamespace, NVS_READONLY, &handle) != ESP_OK) return false;
    uint8_t requested = 0;
    const esp_err_t error = nvs_get_u8(handle, kWifiProvisioningRequestedKey, &requested);
    nvs_close(handle);
    return error == ESP_OK && requested == 1;
}

Status SetWifiProvisioningRequested(bool requested) {
    nvs_handle_t handle = 0;
    if (const esp_err_t error = OpenSecretNamespace(kWifiNamespace, NVS_READWRITE, &handle); error != ESP_OK) {
        return EspError("打开 Wi-Fi 配网请求存储", error);
    }
    const esp_err_t write_error = nvs_set_u8(handle, kWifiProvisioningRequestedKey, requested ? 1 : 0);
    const esp_err_t commit_error = write_error == ESP_OK ? nvs_commit(handle) : write_error;
    nvs_close(handle);
    return commit_error == ESP_OK ? Status::Ok() : EspError("保存 Wi-Fi 配网请求", commit_error);
}

Status PrepareWifiForProvisioning() {
    if (const esp_err_t error = esp_netif_init(); error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
        return EspError("初始化 esp_netif", error);
    }
    if (const esp_err_t error = esp_event_loop_create_default(); error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
        return EspError("初始化 ESP 事件循环", error);
    }
    if (esp_netif_get_handle_from_ifkey("WIFI_STA_DEF") == nullptr && esp_netif_create_default_wifi_sta() == nullptr) {
        return Status::Error(ErrorCode::kUnavailable, "创建 Wi-Fi STA netif 失败");
    }
    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    if (const esp_err_t error = esp_wifi_init(&init_config); error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
        return EspError("初始化 ESP Wi-Fi", error);
    }
    if (const esp_err_t error = esp_wifi_set_mode(WIFI_MODE_STA); error != ESP_OK) {
        return EspError("设置 Wi-Fi STA 模式", error);
    }
    if (const esp_err_t error = esp_wifi_start(); error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
        return EspError("启动 ESP Wi-Fi", error);
    }
    return Status::Ok();
}

Result<std::vector<std::string>> ScanWifiSsidNames() {
    wifi_scan_config_t scan_config{};
    if (const esp_err_t error = esp_wifi_scan_start(&scan_config, true); error != ESP_OK) {
        return Result<std::vector<std::string>>::Failure(ErrorCode::kUnavailable, "扫描 Wi-Fi 失败");
    }
    uint16_t count = 0;
    if (const esp_err_t error = esp_wifi_scan_get_ap_num(&count); error != ESP_OK) {
        return Result<std::vector<std::string>>::Failure(ErrorCode::kUnavailable, "读取 Wi-Fi 扫描数量失败");
    }
    std::vector<wifi_ap_record_t> records(count);
    if (count > 0 && esp_wifi_scan_get_ap_records(&count, records.data()) != ESP_OK) {
        return Result<std::vector<std::string>>::Failure(ErrorCode::kUnavailable, "读取 Wi-Fi 扫描结果失败");
    }
    std::vector<std::string> names;
    names.reserve(count);
    for (const wifi_ap_record_t& record : records) {
        const size_t size = strnlen(reinterpret_cast<const char*>(record.ssid), sizeof(record.ssid));
        if (size == 0) continue;
        const std::string name(reinterpret_cast<const char*>(record.ssid), size);
        if (std::find(names.begin(), names.end(), name) == names.end()) names.push_back(name);
    }
    return Result<std::vector<std::string>>::Success(std::move(names));
}

Result<WifiProvisioningCredentials> GetSoftApCandidate(WifiProvisioningCause cause,
                                                       const WifiProvisioningStatusSink& status_sink) {
    auto names = ScanWifiSsidNames();
    if (!names.ok() || !names.value.has_value()) {
        return Result<WifiProvisioningCredentials>::Failure(names.status.code, names.status.message);
    }
    return ProvisionWifiOverSoftAp(cause, *names.value, status_sink);
}

Status EnsureWifiStaConnected(const WifiProvisioningStatusSink& status_sink) {
    // OTA retries and WebSocket reconnects share this entry point. Do not
    // reapply credentials or call esp_wifi_connect while an association is
    // still usable: doing so creates needless roam churn during a voice turn.
    wifi_ap_record_t active_access_point{};
    if (!IsWifiProvisioningRequested() && esp_wifi_sta_get_ap_info(&active_access_point) == ESP_OK) {
        return Status::Ok();
    }
    const bool force_provisioning = IsWifiProvisioningRequested();
    auto stored_credentials = LoadWifiCredentials();
    bool candidate_requires_persistence = false;
    WifiCredentials credentials;
    WifiProvisioningCause provisioning_cause = WifiProvisioningCause::kMissingCredentials;
    if (!force_provisioning && stored_credentials.ok() && stored_credentials.value.has_value()) {
        credentials = std::move(*stored_credentials.value);
    } else {
        if (stored_credentials.status.code != ErrorCode::kNotFound && !force_provisioning)
            return stored_credentials.status;
        // 物理长按只请求下一次启动进入配网。开始流程时立即消费标记，
        // 这样用户取消、热点超时或候选网络失败后仍能在下次启动恢复旧网络。
        if (force_provisioning) {
            if (const Status consumed = SetWifiProvisioningRequested(false); !consumed.ok()) return consumed;
        }
        if (const Status prepared = PrepareWifiForProvisioning(); !prepared.ok()) return prepared;
        provisioning_cause =
            force_provisioning ? WifiProvisioningCause::kUserRequested : WifiProvisioningCause::kMissingCredentials;
        auto candidate = GetSoftApCandidate(provisioning_cause, status_sink);
        if (!candidate.ok() || !candidate.value.has_value()) {
            // 保留串口作为无显示/诊断环境中的最后回退；热点超时不会清除任何密钥。
            const Status serial = ProvisionWifiCredentialsFromConsole();
            if (!serial.ok()) return candidate.status;
            stored_credentials = LoadWifiCredentials();
            if (!stored_credentials.ok() || !stored_credentials.value.has_value()) return stored_credentials.status;
            credentials = std::move(*stored_credentials.value);
        } else {
            credentials = {.ssid = std::move(candidate.value->ssid), .password = std::move(candidate.value->password)};
            candidate_requires_persistence = true;
        }
    }
    static EventGroupHandle_t events = nullptr;
    static bool initialized = false;
    if (!initialized) {
        if (const Status prepared = PrepareWifiForProvisioning(); !prepared.ok()) return prepared;
        events = xEventGroupCreate();
        if (events == nullptr) return Status::Error(ErrorCode::kUnavailable, "创建 Wi-Fi 事件组失败");
        if (esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &OnWifiEvent, &events) != ESP_OK ||
            esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &OnWifiEvent, &events) != ESP_OK) {
            return Status::Error(ErrorCode::kUnavailable, "注册 Wi-Fi 事件处理器失败");
        }
        if (const esp_err_t error = esp_wifi_set_storage(WIFI_STORAGE_RAM); error != ESP_OK) {
            return EspError("设置 Wi-Fi RAM 存储", error);
        }
        // Voice turns maintain an interactive WSS stream. Disable modem power
        // save so a weak but associated AP cannot defer beacons long enough to
        // turn an otherwise recoverable transport retry into a STA disconnect.
        if (const esp_err_t error = esp_wifi_set_ps(WIFI_PS_NONE); error != ESP_OK) {
            return EspError("关闭 Wi-Fi 省电模式", error);
        }
        initialized = true;
    }

    for (int portal_attempt = 0; portal_attempt < kOtaAttempts; ++portal_attempt) {
        wifi_config_t config{};
        std::memcpy(config.sta.ssid, credentials.ssid.data(),
                    std::min(credentials.ssid.size(), sizeof(config.sta.ssid) - 1));
        std::memcpy(config.sta.password, credentials.password.data(),
                    std::min(credentials.password.size(), sizeof(config.sta.password) - 1));
        config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
        config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
        config.sta.failure_retry_cnt = 2;
        if (const esp_err_t error = esp_wifi_set_mode(WIFI_MODE_STA); error != ESP_OK)
            return EspError("设置 Wi-Fi STA 模式", error);
        if (const esp_err_t error = esp_wifi_set_config(WIFI_IF_STA, &config); error != ESP_OK)
            return EspError("设置 Wi-Fi STA 配置", error);
        if (const esp_err_t error = esp_wifi_start(); error != ESP_OK && error != ESP_ERR_INVALID_STATE)
            return EspError("启动 ESP Wi-Fi", error);
        bool connected = false;
        const int64_t connect_deadline_us = esp_timer_get_time() + static_cast<int64_t>(kWifiConnectWindowMs) * 1000;
        while (esp_timer_get_time() < connect_deadline_us) {
            xEventGroupClearBits(events, kWifiConnectedBit | kWifiFailedBit);
            if (const esp_err_t error = esp_wifi_connect(); error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
                return EspError("连接 Wi-Fi STA", error);
            }
            const int64_t remaining_us = connect_deadline_us - esp_timer_get_time();
            const int wait_ms =
                static_cast<int>(std::min<int64_t>(kWifiConnectTimeoutMs, std::max<int64_t>(1, remaining_us / 1000)));
            const EventBits_t result = xEventGroupWaitBits(events, kWifiConnectedBit | kWifiFailedBit, pdFALSE, pdFALSE,
                                                           pdMS_TO_TICKS(wait_ms));
            wifi_ap_record_t access_point{};
            if ((result & kWifiConnectedBit) != 0 && esp_wifi_sta_get_ap_info(&access_point) == ESP_OK) {
                connected = true;
                break;
            }
            if (esp_timer_get_time() < connect_deadline_us) vTaskDelay(pdMS_TO_TICKS(kWifiRetryDelayMs));
        }
        if (connected) {
            if (candidate_requires_persistence) {
                const Status stored = StoreWifiCredentials(credentials.ssid, credentials.password);
                if (!stored.ok()) return stored;
            }
            return Status::Ok();
        }
        if (status_sink) status_sink("配网", "无法连接该 Wi-Fi，请重新输入");
        // 已保存的凭据不可用时，优先给物理串口一个短恢复窗口。这样现场
        // 维护可以直接写入已确认的 SSID，不必等待 SoftAP 浏览器配网超时。
        // 正常启动不会走到这里，窗口超时或坏帧后仍保持原 SoftAP 回退。
        const Status serial = ProvisionWifiCredentialsFromConsole(kRecoverySerialProvisionTimeoutMs);
        if (serial.ok()) {
            stored_credentials = LoadWifiCredentials();
            if (!stored_credentials.ok() || !stored_credentials.value.has_value()) return stored_credentials.status;
            credentials = std::move(*stored_credentials.value);
            candidate_requires_persistence = false;
            continue;
        }
        if (serial.code != ErrorCode::kNotFound) {
            ESP_LOGW(kTag, "串口 Wi-Fi 恢复失败，回退热点: %s", serial.message.c_str());
        }
        auto candidate = GetSoftApCandidate(WifiProvisioningCause::kConnectionFailed, status_sink);
        if (!candidate.ok() || !candidate.value.has_value()) return candidate.status;
        credentials = {.ssid = std::move(candidate.value->ssid), .password = std::move(candidate.value->password)};
        candidate_requires_persistence = true;
    }
    return Status::Error(ErrorCode::kUnavailable, "Wi-Fi STA 连接失败");
}

Status EspError(const char* operation, esp_err_t error) {
    return Status::Error(ErrorCode::kUnavailable, std::string(operation) + " 失败，esp_err_t=" + std::to_string(error));
}

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

Result<std::string> LoadOrCreateClientId() {
    nvs_handle_t handle = 0;
    if (const esp_err_t error = OpenSecretNamespace(kLinxNamespace, NVS_READWRITE, &handle); error != ESP_OK) {
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

std::string DeviceId() {
    uint8_t mac[6]{};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) return {};
    char value[18]{};
    std::snprintf(value, sizeof(value), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4],
                  mac[5]);
    return value;
}

std::string HexDigest(const uint8_t* digest, size_t size) {
    std::string result;
    result.reserve(size * 2);
    constexpr char kHex[] = "0123456789abcdef";
    for (size_t index = 0; index < size; ++index) {
        result.push_back(kHex[digest[index] >> 4U]);
        result.push_back(kHex[digest[index] & 0x0fU]);
    }
    return result;
}

#include "linx_ota_device.inc"

Result<linx::LinxOtaResponse> FetchOtaResponse(const linx::LinxOtaHttpRequest& request) {
    std::string body;
    esp_http_client_config_t config{};
    config.url = request.url.c_str();
    config.method = HTTP_METHOD_POST;
    config.timeout_ms = 10000;
    config.disable_auto_redirect = true;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.event_handler = CollectOtaResponse;
    config.user_data = &body;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr)
        return Result<linx::LinxOtaResponse>::Failure(ErrorCode::kUnavailable, "Linx OTA HTTP 初始化失败");
    esp_err_t error = ESP_OK;
    for (const auto& header : request.headers) {
        error = esp_http_client_set_header(client, header.name.c_str(), header.value.c_str());
        if (error != ESP_OK) break;
    }
    if (error == ESP_OK) error = esp_http_client_set_post_field(client, request.body.data(), request.body.size());
    if (error == ESP_OK) error = esp_http_client_perform(client);
    const int status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (error != ESP_OK) {
        return Result<linx::LinxOtaResponse>::Failure(ErrorCode::kUnavailable, "Linx OTA HTTPS 请求失败");
    }
    if (status_code != 200) {
        return Result<linx::LinxOtaResponse>::Failure(ErrorCode::kUnavailable, "Linx OTA 返回非成功状态");
    }
    return linx::ParseLinxOtaResponse(body);
}

Status PersistConnectionConfig(const linx::LinxConnectionConfig& config, std::string_view token) {
#if !CONFIG_NVS_ENCRYPTION
    (void)config;
    (void)token;
    return Status::Error(ErrorCode::kUnavailable, "Linx token 存储需要启用 NVS encryption");
#else
    nvs_handle_t handle = 0;
    if (const esp_err_t error = OpenSecretNamespace(kLinxNamespace, NVS_READWRITE, &handle); error != ESP_OK) {
        return EspError("打开 Linx NVS", error);
    }
    esp_err_t error = nvs_set_str(handle, kTokenKey, std::string(token).c_str());
    if (error == ESP_OK) error = nvs_set_str(handle, "websocket_url", config.websocket_url.c_str());
    if (error == ESP_OK) error = nvs_set_str(handle, "token_ref", config.token_ref.c_str());
    if (error == ESP_OK) error = nvs_set_str(handle, "device_id", config.device_id.c_str());
    if (error == ESP_OK) error = nvs_set_str(handle, "client_id", config.client_id.c_str());
    if (error == ESP_OK) error = nvs_commit(handle);
    nvs_close(handle);
    return error == ESP_OK ? Status::Ok() : EspError("保存 Linx OTA 配置", error);
#endif
}

void LogOtaResponseShape(const linx::LinxOtaResponse& response) {
    const bool has_websocket = response.websocket.has_value();
    const bool has_wss_url = has_websocket && response.websocket->url.rfind("wss://", 0) == 0;
    const bool has_token = has_websocket && response.websocket->token.has_value();
    ESP_LOGI(kTag, "LINX_OTA_RESPONSE activation=%d websocket=%d wss_url=%d token=%d",
             response.activation.has_value() ? 1 : 0, has_websocket ? 1 : 0, has_wss_url ? 1 : 0, has_token ? 1 : 0);
}

}  // namespace

const char* LinxSecretPartitionLabel() { return kSecretPartition; }

bool LinxWifiStaConnected() {
#ifdef ESP_PLATFORM
    wifi_ap_record_t access_point{};
    return esp_wifi_sta_get_ap_info(&access_point) == ESP_OK;
#else
    return false;
#endif
}

Status RequestLinxWifiProvisioning() {
#ifdef ESP_PLATFORM
    const Status request = SetWifiProvisioningRequested(true);
    if (!request.ok()) {
        ESP_LOGW(kTag, "WIFI_REPROVISION_REQUEST_FAILED code=%d detail=%s", static_cast<int>(request.code),
                 request.message.c_str());
        return request;
    }
    ESP_LOGI(kTag, "WIFI_REPROVISION_REQUESTED=1");
    esp_restart();
    return Status::Ok();
#else
    return Status::Error(ErrorCode::kUnavailable, "仅 ESP 设备支持 Wi-Fi 配网请求");
#endif
}

Status InitializeLinxSecretStore() {
#if !CONFIG_NVS_ENCRYPTION || !CONFIG_NVS_SEC_KEY_PROTECT_USING_HMAC
    return Status::Error(ErrorCode::kUnavailable, "Linx 凭据存储需要 HMAC NVS encryption");
#else
    nvs_sec_scheme_t* scheme = nvs_flash_get_default_security_scheme();
    if (scheme == nullptr) {
        return Status::Error(ErrorCode::kUnavailable, "HMAC NVS 安全方案未注册");
    }
    nvs_sec_cfg_t encryption_config{};
    esp_err_t error = nvs_flash_read_security_cfg_v2(scheme, &encryption_config);
    if (error == ESP_OK) error = nvs_flash_secure_init_partition(kSecretPartition, &encryption_config);
    std::memset(&encryption_config, 0, sizeof(encryption_config));
    return error == ESP_OK ? Status::Ok() : EspError("初始化 Linx 加密凭据分区", error);
#endif
}

Result<linx::LinxConnectionConfig> BootstrapLinxOtaConfig(std::string_view board_identity,
                                                          const WifiProvisioningStatusSink& provisioning_status_sink) {
    Result<linx::LinxConnectionConfig> last_failure =
        Result<linx::LinxConnectionConfig>::Failure(ErrorCode::kUnavailable, "Linx OTA 初始化失败");
    for (int attempt = 1; attempt <= kOtaBootstrapAttempts; ++attempt) {
        auto device = ReadOtaDeviceInfo(board_identity, provisioning_status_sink);
        if (!device.ok() || !device.value.has_value()) {
            last_failure = Result<linx::LinxConnectionConfig>::Failure(device.status.code, device.status.message);
        } else {
            auto request = linx::BuildLinxOtaRequest(*device.value);
            if (!request.ok() || !request.value.has_value()) {
                last_failure = Result<linx::LinxConnectionConfig>::Failure(request.status.code, request.status.message);
            } else {
                auto response = FetchOtaResponse(*request.value);
                if (response.ok() && response.value.has_value()) {
                    LogOtaResponseShape(*response.value);
                    // 用服务端时间初始化系统时钟（UTC epoch，供空闲态显示 HH:MM）。
                    if (response.value->server_time.has_value() && response.value->server_time->timestamp_ms > 0) {
                        timespec spec{};
                        spec.tv_sec = static_cast<time_t>(response.value->server_time->timestamp_ms / 1000ULL);
                        spec.tv_nsec =
                            static_cast<long>((response.value->server_time->timestamp_ms % 1000ULL) * 1000000ULL);
                        if (clock_settime(CLOCK_REALTIME, &spec) == 0) {
                            // 设置进程时区：CLOCK_REALTIME 保持 UTC（日程比较不受影响），
                            // 由 TZ 让 localtime_r 渲染服务器提供的本地偏移。POSIX 符号与
                            // 分钟偏移相反（UTC+8 → "UTC-8:00"）。缺省按中国时区 +480。
                            const int32_t offset_minutes =
                                response.value->server_time->timezone_offset_minutes.value_or(480);
                            const int32_t absolute = offset_minutes < 0 ? -offset_minutes : offset_minutes;
                            char timezone[24]{};
                            std::snprintf(timezone, sizeof(timezone), "UTC%c%ld:%02ld", offset_minutes >= 0 ? '-' : '+',
                                          static_cast<long>(absolute / 60), static_cast<long>(absolute % 60));
                            setenv("TZ", timezone, 1);
                            tzset();
                            ESP_LOGI(kTag, "LINX_SERVER_TIME_SET=1 tz=%s", timezone);
                        }
                    }
                    if (response.value->activation.has_value()) {
                        // 激活码是服务端明确下发、供操作者在 Linx 控制台绑定本机
                        // 设备的一次性公开信息。仅接受 6 位数字，绝不记录 challenge、
                        // WSS 地址、token、Wi-Fi 配置或 NVS 内容。
                        const std::string& code = response.value->activation->code;
                        const bool six_digit_code =
                            code.size() == 6 && std::all_of(code.begin(), code.end(), [](unsigned char value) {
                                return std::isdigit(value) != 0;
                            });
                        if (six_digit_code) {
                            ESP_LOGW(kTag, "LINX_ACTIVATION_CODE=%s", code.c_str());
                        } else {
                            ESP_LOGW(kTag, "LINX_ACTIVATION_CODE_INVALID=1");
                        }
                        ESP_LOGW(kTag, "LINX_ACTIVATION_REQUIRED=1");
                        return Result<linx::LinxConnectionConfig>::Failure(ErrorCode::kUnavailable,
                                                                           "Linx 设备需要控制台激活");
                    }
                    auto config = linx::BuildLinxConnectionConfig(*response.value, device.value->device_id,
                                                                  device.value->client_id, kTokenReference);
                    if (config.ok() && config.value.has_value()) {
                        const Status persisted =
                            PersistConnectionConfig(*config.value, *response.value->websocket->token);
                        if (persisted.ok()) return config;
                        last_failure = Result<linx::LinxConnectionConfig>::Failure(persisted.code, persisted.message);
                    } else {
                        last_failure =
                            Result<linx::LinxConnectionConfig>::Failure(config.status.code, config.status.message);
                    }
                } else {
                    last_failure =
                        Result<linx::LinxConnectionConfig>::Failure(response.status.code, response.status.message);
                }
            }
        }
        if (attempt < kOtaBootstrapAttempts) {
            const int delay_ms = kOtaRetryDelayMs[static_cast<size_t>(attempt - 1)];
            ESP_LOGW(kTag, "LINX_OTA_RETRY attempt=%d delay_ms=%d", attempt + 1, delay_ms);
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
        }
    }
    return last_failure;
}

}  // namespace voicelife::runtime
