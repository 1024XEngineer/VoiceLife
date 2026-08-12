#include "im_runtime_bootstrap.h"

#include <fcntl.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "linx_ota_bootstrap.h"
#include "nvs.h"
#include "voicelife/im/im_endpoint.h"
#include "voicelife/im/im_provisioning.h"

namespace voicelife::runtime {
namespace {

constexpr char kTag[] = "VoiceLifeIm";
constexpr char kImNamespace[] = "im";
constexpr int kProvisionTimeoutMs = 60000;
constexpr std::size_t kMaximumStoredStringBytes = 1024;
std::atomic_bool g_provisioning_started{false};

Status PrepareProvisioningConsole() {
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    if (!usb_serial_jtag_is_driver_installed()) {
        usb_serial_jtag_driver_config_t config = {
            .tx_buffer_size = 512,
            .rx_buffer_size = 1024,
        };
        if (usb_serial_jtag_driver_install(&config) != ESP_OK) {
            return Status::Error(ErrorCode::kUnavailable, "初始化 USB provisioning 输入缓冲失败");
        }
    }
    usb_serial_jtag_vfs_use_driver();
#endif
    return Status::Ok();
}

void SecureClear(std::string& value) {
    std::fill(value.begin(), value.end(), '\0');
    value.clear();
}

Result<std::string> ReadNvsString(nvs_handle_t handle, std::string_view key) {
    const std::string key_string(key);
    size_t required = 0;
    esp_err_t error = nvs_get_str(handle, key_string.c_str(), nullptr, &required);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        return Result<std::string>::Failure(ErrorCode::kNotFound, "IM NVS 字段缺失");
    }
    if (error != ESP_OK) {
        return Result<std::string>::Failure(ErrorCode::kUnavailable, "读取 IM 加密 NVS 失败");
    }
    if (required <= 1 || required > kMaximumStoredStringBytes + 1) {
        return Result<std::string>::Failure(ErrorCode::kInvalidArgument, "IM NVS 字段为空或越界");
    }
    std::string value(required, '\0');
    error = nvs_get_str(handle, key_string.c_str(), value.data(), &required);
    if (error != ESP_OK) {
        SecureClear(value);
        return Result<std::string>::Failure(ErrorCode::kUnavailable, "读取 IM 加密 NVS 失败");
    }
    value.resize(required - 1);
    return Result<std::string>::Success(std::move(value));
}

bool ReadConsoleBytes(uint8_t* destination, std::size_t size, int timeout_ms) {
    std::size_t received = 0;
    const int64_t deadline_us = esp_timer_get_time() + static_cast<int64_t>(timeout_ms) * 1000;
    const int original_flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (original_flags < 0 || fcntl(STDIN_FILENO, F_SETFL, original_flags | O_NONBLOCK) < 0) return false;

    bool complete = false;
    while (received < size) {
        const ssize_t count = read(STDIN_FILENO, destination + received, size - received);
        if (count > 0) {
            received += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK) break;
        if (esp_timer_get_time() >= deadline_us) break;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    complete = received == size;
    (void)fcntl(STDIN_FILENO, F_SETFL, original_flags);
    return complete;
}

Status StoreProvisioningRequest(im::ImProvisioningRequest& request) {
#if !CONFIG_NVS_ENCRYPTION
    (void)request;
    return Status::Error(ErrorCode::kUnavailable, "IM 凭据存储需要 NVS encryption");
#else
    if (!im::IsHttpsGatewayUrl(request.gateway_origin)) {
        return Status::Error(ErrorCode::kInvalidArgument, "IM Gateway 必须是 HTTPS origin");
    }
    nvs_handle_t handle = 0;
    esp_err_t error = nvs_open_from_partition(LinxSecretPartitionLabel(), kImNamespace, NVS_READWRITE, &handle);
    if (error != ESP_OK) return Status::Error(ErrorCode::kUnavailable, "打开 IM 加密 NVS 失败");

    error = nvs_set_str(handle, "gateway_origin", request.gateway_origin.c_str());
    if (error == ESP_OK) error = nvs_set_str(handle, "device_id", request.device_id.c_str());
    if (error == ESP_OK) error = nvs_set_str(handle, "device_token", request.device_token.c_str());
    if (error == ESP_OK) {
        error = request.user_id.empty() ? nvs_erase_key(handle, "user_id")
                                        : nvs_set_str(handle, "user_id", request.user_id.c_str());
        if (error == ESP_ERR_NVS_NOT_FOUND && request.user_id.empty()) error = ESP_OK;
    }
    if (error == ESP_OK) error = nvs_commit(handle);
    nvs_close(handle);
    return error == ESP_OK ? Status::Ok() : Status::Error(ErrorCode::kUnavailable, "保存 IM 加密 NVS 失败");
#endif
}

Status ProvisionImFromConsole() {
    const Status console_status = PrepareProvisioningConsole();
    if (!console_status.ok()) return console_status;
    ESP_LOGW(kTag, "IM_PROVISION_READY=1 timeout_ms=%d", kProvisionTimeoutMs);
    std::array<uint8_t, im::kImProvisioningHeaderSize> header_bytes{};
    if (!ReadConsoleBytes(header_bytes.data(), header_bytes.size(), kProvisionTimeoutMs)) {
        return Status::Error(ErrorCode::kNotFound, "未收到物理串口 IM provisioning 请求");
    }
    auto header = im::ParseImProvisioningHeader(header_bytes);
    if (!header.ok() || !header.value.has_value()) return header.status;

    std::vector<uint8_t> frame(header_bytes.begin(), header_bytes.end());
    frame.resize(header_bytes.size() + header.value->payload_size);
    if (!ReadConsoleBytes(frame.data() + header_bytes.size(), header.value->payload_size, kProvisionTimeoutMs)) {
        std::fill(frame.begin(), frame.end(), 0);
        return Status::Error(ErrorCode::kInvalidArgument, "物理串口 IM provisioning 内容不完整");
    }
    auto request = im::ParseImProvisioningRequest(frame);
    std::fill(frame.begin(), frame.end(), 0);
    if (!request.ok() || !request.value.has_value()) return request.status;

    const Status status = StoreProvisioningRequest(*request.value);
    SecureClear(request.value->device_token);
    if (status.ok()) ESP_LOGI(kTag, "IM_PROVISIONED=1");
    return status;
}

void ProvisioningTask(void*) {
    const Status status = ProvisionImFromConsole();
    if (!status.ok()) {
        ESP_LOGW(kTag, "IM_PROVISION_FAILED code=%d", static_cast<int>(status.code));
        g_provisioning_started.store(false);
        vTaskDelete(nullptr);
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
}

}  // namespace

Result<std::string> NvsImSecretStore::Read(std::string_view key) {
#if !CONFIG_NVS_ENCRYPTION
    (void)key;
    return Result<std::string>::Failure(ErrorCode::kUnavailable, "IM 凭据读取需要 NVS encryption");
#else
    nvs_handle_t handle = 0;
    const esp_err_t error = nvs_open_from_partition(LinxSecretPartitionLabel(), kImNamespace, NVS_READONLY, &handle);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        return Result<std::string>::Failure(ErrorCode::kNotFound, "IM NVS namespace 未配置");
    }
    if (error != ESP_OK) {
        return Result<std::string>::Failure(ErrorCode::kUnavailable, "打开 IM 加密 NVS 失败");
    }
    auto result = ReadNvsString(handle, key);
    nvs_close(handle);
    return result;
#endif
}

bool EspImRuntimeReadiness::NetworkReady() const {
    wifi_ap_record_t access_point{};
    if (esp_wifi_sta_get_ap_info(&access_point) != ESP_OK) return false;
    esp_netif_t* station = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (station == nullptr) return false;
    esp_netif_dns_info_t dns{};
    if (esp_netif_get_dns_info(station, ESP_NETIF_DNS_MAIN, &dns) != ESP_OK) return false;
    if (dns.ip.type == ESP_IPADDR_TYPE_V4) return dns.ip.u_addr.ip4.addr != 0;
    return dns.ip.u_addr.ip6.addr[0] != 0 || dns.ip.u_addr.ip6.addr[1] != 0 || dns.ip.u_addr.ip6.addr[2] != 0 ||
           dns.ip.u_addr.ip6.addr[3] != 0;
}

bool EspImRuntimeReadiness::SystemTimeReady() const { return im::IsTrustedSystemTime(time(nullptr)); }

Status SynchronizeSystemTime() {
    // 时间同步仅依赖局域网可达性（DHCP NTP / 池化服务器），与 IM 凭据无关；
    // 失败时只降级 IM，绝不阻塞本地语音、唤醒或音频。
    if (EspImRuntimeReadiness().SystemTimeReady()) {
        return Status::Ok();
    }
    // 优先使用 DHCP 下发的 NTP 服务器；未下发时回退到公网池化服务器。
    // wait_for_sync=false 让 esp_netif_sntp_init 非阻塞返回，本函数随后有界等待
    // 时间同步事件，避免无线慢环境下的启动停滞。
    // servers 数组大小由 CONFIG_LWIP_SNTP_MAX_SERVERS 决定：默认 1 时只放首选
    // 池化服务器（DHCP NTP 优先），profile 显式扩到 2 时追加备选。
#if CONFIG_LWIP_SNTP_MAX_SERVERS >= 2
    constexpr size_t kPoolServerCount = 2;
#else
    constexpr size_t kPoolServerCount = 1;
#endif
    static constexpr const char* kPoolServers[kPoolServerCount] = {
        "time.cloudflare.com",
#if CONFIG_LWIP_SNTP_MAX_SERVERS >= 2
        "pool.ntp.org",
#endif
    };
    esp_sntp_config_t config = {
        .smooth_sync = false,
        .server_from_dhcp = true,
        .wait_for_sync = false,
        .start = true,
        .sync_cb = nullptr,
        .renew_servers_after_new_IP = true,
        .ip_event_to_renew = IP_EVENT_STA_GOT_IP,
        .index_of_first_server = 0,
        .num_of_servers = kPoolServerCount,
#if CONFIG_LWIP_SNTP_MAX_SERVERS >= 2
        .servers = {kPoolServers[0], kPoolServers[1]},
#else
        .servers = {kPoolServers[0]},
#endif
    };

    const esp_err_t init = esp_netif_sntp_init(&config);
    if (init != ESP_OK) {
        ESP_LOGW(kTag, "SNTP_INIT_FAILED code=%d", static_cast<int>(init));
        return Status::Error(ErrorCode::kUnavailable, "SNTP 初始化失败");
    }
    // 有界等待：SNTP 在 Wi-Fi 已关联的网络下通常 1~2s 内完成；10s 后仍未同步
    // 则降级，等下次开机再试，不让 IM 卡住设备启动。
    constexpr TickType_t kSyncWaitTicks = pdMS_TO_TICKS(10 * 1000);
    if (esp_netif_sntp_sync_wait(kSyncWaitTicks) != ESP_OK) {
        ESP_LOGW(kTag, "SNTP_SYNC_TIMEOUT=1");
        return Status::Error(ErrorCode::kUnavailable, "等待 SNTP 时间同步超时");
    }
    const time_t now = time(nullptr);
    if (!im::IsTrustedSystemTime(now)) {
        ESP_LOGW(kTag, "SNTP_SYNC_UNCERTAIN now=%lld", static_cast<long long>(now));
        return Status::Error(ErrorCode::kUnavailable, "SNTP 同步后的时间仍不可信");
    }
    ESP_LOGI(kTag, "SNTP_SYNCED=1");
    return Status::Ok();
}

bool StartImProvisioningTask() {
    bool expected = false;
    if (!g_provisioning_started.compare_exchange_strong(expected, true)) return true;
    if (xTaskCreate(&ProvisioningTask, "voicelife_im_provision", 6144, nullptr, 3, nullptr) != pdPASS) {
        g_provisioning_started.store(false);
        return false;
    }
    return true;
}

}  // namespace voicelife::runtime
