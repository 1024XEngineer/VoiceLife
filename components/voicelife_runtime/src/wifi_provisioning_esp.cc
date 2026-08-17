#include "wifi_provisioning_esp.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

namespace voicelife::runtime {
namespace {

constexpr char kTag[] = "VoiceLifeSoftAp";
constexpr uint32_t kPortalTimeoutMs = 5 * 60 * 1000;
// ssid= (5) + 32 个字节各自最坏 %XX（96）+ &password=（10）+ 64 个字节各自最坏 %XX（192）。
constexpr size_t kMaxFormBytes = 303;
constexpr uint16_t kDnsPort = 53;
constexpr std::array<uint8_t, 4> kPortalAddress = {192, 168, 4, 1};

std::string EscapeHtml(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (const char character : value) {
        switch (character) {
            case '&':
                escaped += "&amp;";
                break;
            case '<':
                escaped += "&lt;";
                break;
            case '>':
                escaped += "&gt;";
                break;
            case '"':
                escaped += "&quot;";
                break;
            case '\'':
                escaped += "&#39;";
                break;
            default:
                escaped.push_back(character);
                break;
        }
    }
    return escaped;
}

std::string PortalPage(const std::vector<std::string>& ssids) {
    std::string options;
    for (const std::string& ssid : ssids) {
        const std::string safe_ssid = EscapeHtml(ssid);
        options += "<option value=\"" + safe_ssid + "\">" + safe_ssid + "</option>";
    }
    return "<!doctype html><html lang=\"zh-CN\"><meta charset=\"utf-8\"><meta name=\"viewport\" "
           "content=\"width=device-width,initial-scale=1\"><title>VoiceLife 配网</title><style>body{max-width:30rem;"
           "margin:2rem auto;padding:0 1rem;font:16px sans-serif}input,select,button{box-sizing:border-box;width:100%;"
           "padding:.7rem;margin:.4rem 0}button{background:#1769aa;color:#fff;border:0;border-radius:.3rem}</style>"
           "<h1>连接家庭 Wi-Fi</h1><p>输入密码后，设备会先验证网络；验证成功才会保存。</p>"
           "<form method=\"post\" action=\"/configure\"><label>网络名称</label><input name=\"ssid\" list=\"networks\" "
           "maxlength=\"32\" required><datalist id=\"networks\">" +
           options +
           "</datalist><label>密码</label><input name=\"password\" type=\"password\" maxlength=\"64\" required>"
           "<button type=\"submit\">连接</button></form></html>";
}

struct CaptivePortal {
    WifiProvisioningSession session;
    SemaphoreHandle_t mutex = nullptr;
    httpd_handle_t server = nullptr;
    int dns_socket = -1;
    TaskHandle_t dns_task = nullptr;
    volatile bool dns_running = false;
    std::string page;
};

void SendHtml(httpd_req_t* request, const std::string& page) {
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    (void)httpd_resp_send(request, page.data(), page.size());
}

esp_err_t PortalGetHandler(httpd_req_t* request) {
    auto* portal = static_cast<CaptivePortal*>(request->user_ctx);
    SendHtml(request, portal->page);
    return ESP_OK;
}

esp_err_t PortalConfigureHandler(httpd_req_t* request) {
    auto* portal = static_cast<CaptivePortal*>(request->user_ctx);
    if (request->content_len <= 0 || request->content_len > static_cast<int>(kMaxFormBytes)) {
        httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "invalid form");
        return ESP_FAIL;
    }
    std::string body(static_cast<size_t>(request->content_len), '\0');
    size_t offset = 0;
    while (offset < body.size()) {
        const int received = httpd_req_recv(request, body.data() + offset, body.size() - offset);
        if (received <= 0) {
            httpd_resp_send_err(request, HTTPD_408_REQ_TIMEOUT, "request timeout");
            return ESP_FAIL;
        }
        offset += static_cast<size_t>(received);
    }
    auto credentials = ParseWifiProvisioningForm(body);
    if (!credentials.has_value()) {
        httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "invalid form");
        return ESP_FAIL;
    }
    bool accepted = false;
    if (xSemaphoreTake(portal->mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        accepted = portal->session.Submit(std::move(*credentials));
        xSemaphoreGive(portal->mutex);
    }
    if (!accepted) {
        httpd_resp_set_status(request, "409 Conflict");
        httpd_resp_sendstr(request, "busy");
        return ESP_FAIL;
    }
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_sendstr(request, "<!doctype html><meta charset=\"utf-8\"><p>正在验证网络，请稍候。</p>");
    return ESP_OK;
}

void DnsTaskEntry(void* context) {
    auto* portal = static_cast<CaptivePortal*>(context);
    std::array<uint8_t, 512> request{};
    std::array<uint8_t, 512> response{};
    while (portal->dns_running) {
        sockaddr_in client{};
        socklen_t client_size = sizeof(client);
        const int received = lwip_recvfrom(portal->dns_socket, request.data(), request.size(), 0,
                                           reinterpret_cast<sockaddr*>(&client), &client_size);
        if (received < 12) continue;
        size_t question_end = 12;
        while (question_end < static_cast<size_t>(received) && request[question_end] != 0) {
            question_end += static_cast<size_t>(request[question_end]) + 1;
        }
        if (question_end + 5 > static_cast<size_t>(received)) continue;
        question_end += 5;  // terminating zero and QTYPE/QCLASS
        constexpr size_t kAnswerBytes = 16;
        if (question_end + kAnswerBytes > response.size()) continue;
        std::memcpy(response.data(), request.data(), question_end);
        response[2] = 0x81;
        response[3] = 0x80;
        response[6] = 0;
        response[7] = 1;
        response[8] = response[9] = response[10] = response[11] = 0;
        size_t answer = question_end;
        response[answer++] = 0xc0;
        response[answer++] = 0x0c;
        response[answer++] = 0;
        response[answer++] = 1;
        response[answer++] = 0;
        response[answer++] = 1;
        response[answer++] = 0;
        response[answer++] = 0;
        response[answer++] = 0;
        response[answer++] = 30;
        response[answer++] = 0;
        response[answer++] = 4;
        for (const uint8_t octet : kPortalAddress) response[answer++] = octet;
        (void)lwip_sendto(portal->dns_socket, response.data(), answer, 0, reinterpret_cast<sockaddr*>(&client),
                          client_size);
    }
    portal->dns_task = nullptr;
    vTaskDelete(nullptr);
}

Status StartDns(CaptivePortal* portal) {
    portal->dns_socket = lwip_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (portal->dns_socket < 0) return Status::Error(ErrorCode::kUnavailable, "创建 SoftAP DNS socket 失败");
    const timeval timeout = {.tv_sec = 0, .tv_usec = 200000};
    (void)lwip_setsockopt(portal->dns_socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(kDnsPort);
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (lwip_bind(portal->dns_socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        lwip_close(portal->dns_socket);
        portal->dns_socket = -1;
        return Status::Error(ErrorCode::kUnavailable, "绑定 SoftAP DNS 端口失败");
    }
    portal->dns_running = true;
    if (xTaskCreate(&DnsTaskEntry, "voicelife_dns", 3072, portal, 4, &portal->dns_task) != pdPASS) {
        portal->dns_running = false;
        lwip_close(portal->dns_socket);
        portal->dns_socket = -1;
        return Status::Error(ErrorCode::kUnavailable, "创建 SoftAP DNS 任务失败");
    }
    return Status::Ok();
}

void StopPortal(CaptivePortal* portal) {
    if (portal->server != nullptr) {
        (void)httpd_stop(portal->server);
        portal->server = nullptr;
    }
    portal->dns_running = false;
    if (portal->dns_socket >= 0) {
        lwip_close(portal->dns_socket);
        portal->dns_socket = -1;
    }
    for (int attempt = 0; portal->dns_task != nullptr && attempt < 20; ++attempt) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (portal->dns_task != nullptr) {
        vTaskDelete(portal->dns_task);
        portal->dns_task = nullptr;
    }
    if (portal->mutex != nullptr) {
        vSemaphoreDelete(portal->mutex);
        portal->mutex = nullptr;
    }
    (void)esp_wifi_set_mode(WIFI_MODE_STA);
}

std::pair<std::string, std::string> MakeSoftApIdentity() {
    uint8_t mac[6]{};
    (void)esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    char ssid[20]{};
    char password[12]{};
    std::snprintf(ssid, sizeof(ssid), "VoiceLife-%02X%02X%02X", mac[3], mac[4], mac[5]);
    std::snprintf(password, sizeof(password), "%08lX", static_cast<unsigned long>(esp_random()));
    return {ssid, password};
}

}  // namespace

Result<WifiProvisioningCredentials> ProvisionWifiOverSoftAp(WifiProvisioningCause cause,
                                                            const std::vector<std::string>& discovered_ssids,
                                                            const WifiProvisioningStatusSink& status_sink) {
    CaptivePortal portal;
    portal.mutex = xSemaphoreCreateMutex();
    if (portal.mutex == nullptr) {
        return Result<WifiProvisioningCredentials>::Failure(ErrorCode::kUnavailable, "创建 SoftAP 配网锁失败");
    }
    portal.session.Start(cause);
    portal.page = PortalPage(discovered_ssids);

    esp_netif_t* access_point = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (access_point == nullptr && esp_netif_create_default_wifi_ap() == nullptr) {
        StopPortal(&portal);
        return Result<WifiProvisioningCredentials>::Failure(ErrorCode::kUnavailable, "创建 SoftAP netif 失败");
    }
    const auto [ssid, password] = MakeSoftApIdentity();
    wifi_config_t config{};
    std::memcpy(config.ap.ssid, ssid.data(), ssid.size());
    std::memcpy(config.ap.password, password.data(), password.size());
    config.ap.ssid_len = static_cast<uint8_t>(ssid.size());
    config.ap.channel = 1;
    config.ap.max_connection = 2;
    config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    if (const esp_err_t error = esp_wifi_set_mode(WIFI_MODE_APSTA); error != ESP_OK) {
        StopPortal(&portal);
        return Result<WifiProvisioningCredentials>::Failure(ErrorCode::kUnavailable, "启动 SoftAP 失败");
    }
    if (const esp_err_t error = esp_wifi_set_config(WIFI_IF_AP, &config); error != ESP_OK) {
        StopPortal(&portal);
        return Result<WifiProvisioningCredentials>::Failure(ErrorCode::kUnavailable, "配置 SoftAP 失败");
    }
    if (const esp_err_t error = esp_wifi_start(); error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
        StopPortal(&portal);
        return Result<WifiProvisioningCredentials>::Failure(ErrorCode::kUnavailable, "启动 SoftAP 失败");
    }

    httpd_config_t config_http = HTTPD_DEFAULT_CONFIG();
    config_http.uri_match_fn = httpd_uri_match_wildcard;
    if (const esp_err_t error = httpd_start(&portal.server, &config_http); error != ESP_OK) {
        StopPortal(&portal);
        return Result<WifiProvisioningCredentials>::Failure(ErrorCode::kUnavailable, "启动 SoftAP HTTP 服务失败");
    }
    const httpd_uri_t root = {.uri = "/*", .method = HTTP_GET, .handler = &PortalGetHandler, .user_ctx = &portal};
    const httpd_uri_t configure = {
        .uri = "/configure", .method = HTTP_POST, .handler = &PortalConfigureHandler, .user_ctx = &portal};
    if (httpd_register_uri_handler(portal.server, &root) != ESP_OK ||
        httpd_register_uri_handler(portal.server, &configure) != ESP_OK) {
        StopPortal(&portal);
        return Result<WifiProvisioningCredentials>::Failure(ErrorCode::kUnavailable, "注册 SoftAP HTTP 路由失败");
    }
    if (const Status dns = StartDns(&portal); !dns.ok()) {
        StopPortal(&portal);
        return Result<WifiProvisioningCredentials>::Failure(dns.code, dns.message);
    }
    if (status_sink) {
        // OLED 内容栏只有 108px 宽。热点名称可由手机 Wi-Fi 列表识别，密码必须完整可见。
        status_sink("配网密码", password);
    }
    ESP_LOGI(kTag, "SOFTAP_STARTED=1 timeout_ms=%lu", static_cast<unsigned long>(kPortalTimeoutMs));

    const int64_t deadline = esp_timer_get_time() + static_cast<int64_t>(kPortalTimeoutMs) * 1000;
    while (esp_timer_get_time() < deadline) {
        if (xSemaphoreTake(portal.mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            auto credentials = portal.session.TakePendingCredentials();
            xSemaphoreGive(portal.mutex);
            if (credentials.has_value()) {
                StopPortal(&portal);
                return Result<WifiProvisioningCredentials>::Success(std::move(*credentials));
            }
        }
        // 该函数在启动主任务中等待；必须让出 CPU，保证 IDLE 任务能喂狗并让 HTTP/DNS 任务运行。
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    portal.session.Timeout();
    StopPortal(&portal);
    return Result<WifiProvisioningCredentials>::Failure(ErrorCode::kUnavailable, "SoftAP 配网已超时");
}

}  // namespace voicelife::runtime
