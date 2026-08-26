#include "im_runtime_bootstrap.h"

#include <time.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <string>

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "linx_ota_bootstrap.h"
#include "nvs.h"
#include "usb_serial_frame_router.h"
#include "voicelife/im/im_endpoint.h"
#include "voicelife/im/im_pairing_controller.h"
#include "voicelife/im/im_provisioning.h"

namespace voicelife::runtime {
namespace {

constexpr char kTag[] = "VoiceLifeIm";
constexpr char kImNamespace[] = "im";
constexpr int kProvisionTimeoutMs = 60000;
constexpr std::size_t kMaximumStoredStringBytes = 1024;
std::atomic_bool g_provisioning_started{false};
std::atomic<im::ImPairingPort*> g_pairing_client{nullptr};
std::atomic_uint32_t g_pairing_window_generation{0};
std::atomic_bool g_pairing_active{false};
std::string g_pairing_device_id;
std::optional<std::string> g_pairing_user_id;

const char* PairingStatusName(im::PairingFlowStatus status) {
    switch (status) {
        case im::PairingFlowStatus::kPending:
            return "pending";
        case im::PairingFlowStatus::kRetrying:
            return "retrying";
        case im::PairingFlowStatus::kConfirmed:
            return "confirmed";
        case im::PairingFlowStatus::kExpired:
            return "expired";
        case im::PairingFlowStatus::kCancelled:
            return "cancelled";
        case im::PairingFlowStatus::kNotFound:
            return "not_found";
        case im::PairingFlowStatus::kTimedOut:
            return "timed_out";
        case im::PairingFlowStatus::kCredentialRejected:
            return "credential_rejected";
        case im::PairingFlowStatus::kFailed:
            return "failed";
        default:
            return "waiting";
    }
}

Status RunPairingAcceptance(uint8_t expires_in_minutes) {
    im::ImPairingPort* client = g_pairing_client.load(std::memory_order_acquire);
    if (client == nullptr) return Status::Error(ErrorCode::kUnavailable, "IM Runtime 尚未 ready");

    EspPairingClock clock;
    im::PairingSessionController controller(*client, clock);
    const auto begun = controller.Begin({.user_id = g_pairing_user_id, .expires_in_minutes = expires_in_minutes});
    if (begun.status != im::PairingFlowStatus::kPending) {
        ESP_LOGW(kTag, "IM_PAIRING_STATUS=%s", PairingStatusName(begun.status));
        return Status::Error(ErrorCode::kUnavailable, "创建配对会话失败");
    }
    ESP_LOGI(kTag, "IM_PAIRING_SCOPE device_id=%s user_id=%s", g_pairing_device_id.c_str(), g_pairing_user_id->c_str());
    ESP_LOGI(kTag, "IM_PAIRING_CODE=%s expires_at=%s", begun.display_code.c_str(), begun.expires_at.c_str());
    ESP_LOGI(kTag, "IM_PAIRING_STATUS=pending");

    while (controller.active()) {
        vTaskDelay(pdMS_TO_TICKS(100));
        const auto result = controller.Poll();
        if (result.status == im::PairingFlowStatus::kWaiting) continue;
        ESP_LOGI(kTag, "IM_PAIRING_STATUS=%s", PairingStatusName(result.status));
        if (result.status == im::PairingFlowStatus::kCredentialRejected ||
            result.status == im::PairingFlowStatus::kFailed) {
            return Status::Error(ErrorCode::kUnavailable, "配对状态查询失败");
        }
    }
    return Status::Ok();
}

struct PairingTaskArguments {
    uint8_t expires_in_minutes;
};

void PairingTask(void* context) {
    auto* arguments = static_cast<PairingTaskArguments*>(context);
    const uint8_t expires_in_minutes = arguments->expires_in_minutes;
    delete arguments;
    const Status status = RunPairingAcceptance(expires_in_minutes);
    if (!status.ok()) ESP_LOGW(kTag, "IM_PAIRING_FAILED code=%d", static_cast<int>(status.code));
    g_pairing_active.store(false, std::memory_order_release);
    vTaskDelete(nullptr);
}

Status StartPairingAcceptance(std::span<const uint8_t> frame) {
    auto trigger = im::ParseImPairingTrigger(frame);
    if (!trigger.ok() || !trigger.value.has_value()) return trigger.status;
    if (g_pairing_client.load(std::memory_order_acquire) == nullptr || !g_pairing_user_id.has_value()) {
        return Status::Error(ErrorCode::kUnavailable, "IM Runtime 尚未 ready");
    }
    bool expected = false;
    if (!g_pairing_active.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return Status::Error(ErrorCode::kAlreadyExists, "已有配对会话正在轮询");
    }
    auto* arguments = new (std::nothrow) PairingTaskArguments{static_cast<uint8_t>(trigger.value->expires_in_minutes)};
    if (arguments == nullptr) {
        g_pairing_active.store(false, std::memory_order_release);
        return Status::Error(ErrorCode::kInternal, "创建配对轮询参数失败");
    }
    if (xTaskCreate(&PairingTask, "voicelife_im_pairing", 6144, arguments, 3, nullptr) != pdPASS) {
        delete arguments;
        g_pairing_active.store(false, std::memory_order_release);
        return Status::Error(ErrorCode::kInternal, "创建配对轮询任务失败");
    }
    return Status::Ok();
}

struct ConsoleCommandResult {
    Status status;
    bool pairing = false;
    bool restart = false;
};

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

    size_t existing_size = 0;
    error = nvs_get_str(handle, "gateway_origin", nullptr, &existing_size);
    if (!request.allow_overwrite && error == ESP_OK) {
        nvs_close(handle);
        return Status::Error(ErrorCode::kAlreadyExists, "IM 配置已存在；覆盖必须使用 VLI2");
    }
    if (error != ESP_OK && error != ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        return Status::Error(ErrorCode::kUnavailable, "检查已有 IM 配置失败");
    }

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

ConsoleCommandResult ReadImConsoleCommand() {
    const Status router_status = StartUsbSerialFrameRouter();
    if (!router_status.ok()) return {.status = router_status};
    ESP_LOGW(kTag, "IM_PROVISION_READY=1 timeout_ms=%d", kProvisionTimeoutMs);
    if (g_pairing_client.load(std::memory_order_acquire) != nullptr) {
        ESP_LOGW(kTag, "IM_PAIRING_READY=1 timeout_ms=%d", kProvisionTimeoutMs);
    }
    UsbSerialFrame frame;
    if (!ReceiveImUsbSerialFrame(&frame, kProvisionTimeoutMs)) {
        return {.status = Status::Error(ErrorCode::kNotFound, "未收到物理串口 IM 请求")};
    }
    if (frame.kind == UsbSerialFrameKind::kImPairing) {
        const Status status = StartPairingAcceptance(frame.view());
        std::fill(frame.bytes.begin(), frame.bytes.end(), 0);
        return {.status = status, .pairing = true};
    }
    auto request = im::ParseImProvisioningRequest(frame.view());
    std::fill(frame.bytes.begin(), frame.bytes.end(), 0);
    if (!request.ok() || !request.value.has_value()) return {.status = request.status};

    const Status status = StoreProvisioningRequest(*request.value);
    SecureClear(request.value->device_token);
    if (status.ok()) ESP_LOGI(kTag, "IM_PROVISIONED=1");
    return {.status = status, .restart = status.ok()};
}

void ProvisioningTask(void*) {
    uint32_t observed_pairing_window = g_pairing_window_generation.load(std::memory_order_acquire);
    while (true) {
        const ConsoleCommandResult result = ReadImConsoleCommand();
        if (result.restart) {
            vTaskDelay(pdMS_TO_TICKS(300));
            esp_restart();
        }
        if (result.pairing) {
            if (!result.status.ok()) {
                ESP_LOGW(kTag, "IM_PAIRING_FAILED code=%d", static_cast<int>(result.status.code));
            }
            observed_pairing_window = g_pairing_window_generation.load(std::memory_order_acquire);
            // 配对轮询不占用串口读取者；继续开放 provisioning，坏凭据可用 VLI2 现场恢复。
            continue;
        }
        if (!result.status.ok()) {
            ESP_LOGW(kTag, "IM_PROVISION_FAILED code=%d", static_cast<int>(result.status.code));
        }
        const uint32_t requested_pairing_window = g_pairing_window_generation.load(std::memory_order_acquire);
        if (requested_pairing_window != observed_pairing_window &&
            g_pairing_client.load(std::memory_order_acquire) != nullptr) {
            observed_pairing_window = requested_pairing_window;
            continue;
        }
        if (g_pairing_active.load(std::memory_order_acquire)) continue;
        break;
    }
    g_provisioning_started.store(false, std::memory_order_release);
    // RegisterImPairingAcceptance 可能恰好在旧窗口退出时请求新窗口；重新检查代数，避免丢唤醒。
    if (g_pairing_window_generation.load(std::memory_order_acquire) != observed_pairing_window &&
        g_pairing_client.load(std::memory_order_acquire) != nullptr) {
        (void)StartImProvisioningTask();
    }
    vTaskDelete(nullptr);
}

}  // namespace

uint64_t EspPairingClock::MonotonicMillis() const { return static_cast<uint64_t>(esp_timer_get_time() / 1000); }

uint64_t EspPairingClock::UnixMillis() const { return static_cast<uint64_t>(time(nullptr)) * 1000U; }

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

    static std::atomic_bool initialized{false};
    bool expected = false;
    if (initialized.compare_exchange_strong(expected, true)) {
        const esp_err_t init = esp_netif_sntp_init(&config);
        if (init != ESP_OK) {
            initialized.store(false);
            ESP_LOGW(kTag, "SNTP_INIT_FAILED code=%d", static_cast<int>(init));
            return Status::Error(ErrorCode::kUnavailable, "SNTP 初始化失败");
        }
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

void RegisterImPairingAcceptance(im::ImPairingPort* client, std::string device_id, std::optional<std::string> user_id) {
    if (client == nullptr) return;
    if (device_id.empty()) {
        ESP_LOGW(kTag, "IM_PAIRING_UNAVAILABLE=device_id_missing");
        return;
    }
    if (!user_id.has_value() || user_id->empty()) {
        ESP_LOGW(kTag, "IM_PAIRING_UNAVAILABLE=user_id_missing");
        return;
    }
    g_pairing_device_id = std::move(device_id);
    g_pairing_user_id = std::move(user_id);
    g_pairing_client.store(client, std::memory_order_release);
    g_pairing_window_generation.fetch_add(1, std::memory_order_acq_rel);
    if (!StartImProvisioningTask()) {
        ESP_LOGW(kTag, "IM_PAIRING_TASK_FAILED=1");
        return;
    }
    // 任务已存在时不会再次进入 ReadImConsoleCommand，因此在注册点显式发布 ready 标记。
    ESP_LOGW(kTag, "IM_PAIRING_READY=1 timeout_ms=%d", kProvisionTimeoutMs);
}

}  // namespace voicelife::runtime
