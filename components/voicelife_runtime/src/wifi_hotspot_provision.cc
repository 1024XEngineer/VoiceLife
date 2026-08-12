#include "voicelife/runtime/wifi_hotspot_provision.h"

#ifdef ESP_PLATFORM

#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "http_parser.h"
#include "lwip/inet.h"
#include "lwip/ip4_addr.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "nvs.h"

namespace voicelife::runtime {
namespace {

constexpr char kTag[] = "VoiceLifeHotspotProvision";
constexpr char kWifiNamespace[] = "wifi";
constexpr char kWifiSsidKey[] = "ssid";
constexpr char kWifiPasswordKey[] = "password";
constexpr char kApIp[] = "192.168.4.1";
constexpr char kApNetmask[] = "255.255.255.0";
constexpr char kApGateway[] = "192.168.4.1";
constexpr int kMaxSsidLen = 32;
constexpr int kMaxPasswordLen = 64;

// 页面：列出扫描到的 Wi-Fi + 密码表单。POST 到 /api/config。
constexpr char kIndexHtml[] =
    R"HTML(<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>VoiceLife 配网</title>
<style>body{font-family:sans-serif;max-width:480px;margin:20px auto;padding:0 12px}h1{font-size:20px}ul{list-style:none;padding:0}li{padding:8px;border-bottom:1px solid #eee}button{width:100%;padding:10px;margin:4px 0}input{width:100%;padding:8px;box-sizing:border-box}</style></head>
<body><h1>VoiceLife 配网</h1><p>选择 Wi-Fi 并输入密码：</p>
<form method="post" action="/api/config">
<ul>__SSID_LIST__</ul>
<p><input type="password" name="password" placeholder="Wi-Fi 密码" maxlength="64"></p>
<button type="submit">连接</button>
</form></body></html>)HTML";

// 扫描结果 JSON：供页面渲染（简单拼接，SSID 已做 HTML 转义）。
constexpr char kScanJsonPrefix[] = "{\"aps\":[";
constexpr char kScanJsonSuffix[] = "]}";

// 简单 HTML 转义，防注入。
std::string HtmlEscape(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        switch (c) {
            case '&':
                out += "&amp;";
                break;
            case '<':
                out += "&lt;";
                break;
            case '>':
                out += "&gt;";
                break;
            case '"':
                out += "&quot;";
                break;
            case '\'':
                out += "&#39;";
                break;
            default:
                out += c;
                break;
        }
    }
    return out;
}

// 由 URL 编码的 form 提取字段（简单解析，仅处理 ssid/password）。
bool ParseFormValue(const char* body, size_t body_len, const char* field, std::string& out) {
    const std::string_view data(body, body_len);
    const std::string key = std::string(field) + "=";
    const size_t pos = data.find(key);
    if (pos == std::string_view::npos) return false;
    size_t value_start = pos + key.size();
    size_t value_end = data.find('&', value_start);
    if (value_end == std::string_view::npos) value_end = data.size();
    std::string value(data.substr(value_start, value_end - value_start));
    // URL 解码（仅 + 和 %XX）。
    std::string decoded;
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '+') {
            decoded += ' ';
        } else if (value[i] == '%' && i + 2 < value.size()) {
            char hex[3] = {value[i + 1], value[i + 2], '\0'};
            decoded += static_cast<char>(static_cast<int>(strtol(hex, nullptr, 16)));
            i += 2;
        } else {
            decoded += value[i];
        }
    }
    out = std::move(decoded);
    return !out.empty();
}

}  // namespace

// httpd handler 是函数指针（不能捕获 lambda），配置状态用文件级静态变量。
volatile bool g_hotspot_configured = false;
std::string g_hotspot_ssid;
std::string g_hotspot_password;

esp_err_t HandleHotspotIndex(httpd_req_t* req);
esp_err_t HandleHotspotConfig(httpd_req_t* req);

Status RunHotspotProvision(HotspotProvisionResult& result, uint32_t timeout_ms) {
    // 保存当前 STA 状态，结束后由调用方恢复。
    wifi_mode_t original_mode = WIFI_MODE_NULL;
    (void)esp_wifi_get_mode(&original_mode);

    // 1. 创建 AP netif（若已存在则复用）。
    esp_netif_t* ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap_netif == nullptr) {
        ap_netif = esp_netif_create_default_wifi_ap();
    }
    if (ap_netif == nullptr) {
        return Status::Error(ErrorCode::kUnavailable, "创建 Wi-Fi AP netif 失败");
    }
    esp_netif_ip_info_t ip_info{};
    ip_info.ip.addr = ipaddr_addr(kApIp);
    ip_info.netmask.addr = ipaddr_addr(kApNetmask);
    ip_info.gw.addr = ipaddr_addr(kApGateway);
    (void)esp_netif_dhcps_stop(ap_netif);
    (void)esp_netif_set_ip_info(ap_netif, &ip_info);
    (void)esp_netif_dhcps_start(ap_netif);

    // 2. 配置并启动 AP（SSID=Voicelife-XXXX，无密码）。
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    char ssid[32] = {};
    std::snprintf(ssid, sizeof(ssid), "Voicelife-%02X%02X", mac[4], mac[5]);
    wifi_config_t ap_config{};
    std::snprintf(reinterpret_cast<char*>(ap_config.ap.ssid), sizeof(ap_config.ap.ssid), "%s", ssid);
    ap_config.ap.ssid_len = static_cast<uint8_t>(strnlen(ssid, sizeof(ssid)));
    ap_config.ap.channel = 6;
    ap_config.ap.max_connection = 4;
    ap_config.ap.authmode = WIFI_AUTH_OPEN;
    if (const esp_err_t error = esp_wifi_set_mode(WIFI_MODE_APSTA); error != ESP_OK) {
        return Status::Error(ErrorCode::kUnavailable, "设置 APSTA 模式失败 esp_err_t=" + std::to_string(error));
    }
    if (const esp_err_t error = esp_wifi_set_config(WIFI_IF_AP, &ap_config); error != ESP_OK) {
        return Status::Error(ErrorCode::kUnavailable, "设置 AP 配置失败 esp_err_t=" + std::to_string(error));
    }
    if (const esp_err_t error = esp_wifi_start(); error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
        return Status::Error(ErrorCode::kUnavailable, "启动 Wi-Fi AP 失败 esp_err_t=" + std::to_string(error));
    }
    ESP_LOGI(kTag, "HOTSPOT_AP_STARTED ssid=%s ip=%s", ssid, kApIp);

    // 3. 启动 DNS 服务器：劫持所有 DNS 查询到 192.168.4.1（小智同款 DNS 劫持）。
    struct sockaddr_in dns_addr {};
    dns_addr.sin_family = AF_INET;
    dns_addr.sin_port = htons(53);
    dns_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    int dns_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (dns_socket < 0 || bind(dns_socket, reinterpret_cast<struct sockaddr*>(&dns_addr), sizeof(dns_addr)) < 0) {
        ESP_LOGW(kTag, "HOTSPOT_DNS_BIND_FAILED");
        if (dns_socket >= 0) close(dns_socket);
    } else {
        ESP_LOGI(kTag, "HOTSPOT_DNS_STARTED");
        // DNS 应答任务：收到任意 DNS 查询，固定应答 A 记录 -> 192.168.4.1
        // （小智 DNS 劫持同款：让浏览器访问配置页无需记 IP）。
        xTaskCreate(
            [](void* arg) {
                const int fd = *static_cast<int*>(arg);
                uint8_t packet[512] = {};
                struct sockaddr_in client {};
                socklen_t client_len = sizeof(client);
                while (true) {
                    const int received = recvfrom(fd, packet, sizeof(packet), 0,
                                                  reinterpret_cast<struct sockaddr*>(&client), &client_len);
                    if (received < 12) continue;
                    // 构造应答：复制头部，设置 QR/RA，回写相同 question + A 记录。
                    uint8_t response[512] = {};
                    size_t offset = 0;
                    std::memcpy(response + offset, packet, received);
                    offset += static_cast<size_t>(received);
                    // 修改头部：QR=1, RA=1；QDCOUNT 保持，ANCOUNT=1。
                    response[2] |= 0x80;  // QR
                    response[3] |= 0x80;  // RA
                    response[6] = 0;
                    response[7] = 1;  // ANCOUNT=1
                    // 找到 question 末尾（跳过 QNAME）。
                    size_t cursor = 12;
                    while (cursor < static_cast<size_t>(received) && packet[cursor] != 0) {
                        cursor += static_cast<size_t>(packet[cursor]) + 1;
                    }
                    cursor += 5;  // 跳过 0 结尾 + QTYPE/QCLASS
                    // 添加 Answer：NAME 指针 0xC00C, TYPE A, CLASS IN, TTL, RDLEN 4, IP。
                    const uint8_t answer[] = {0xc0, 0x0c, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00,
                                              0x00, 0x3c, 0x00, 0x04, 192,  168,  4,    1};
                    std::memcpy(response + offset, answer, sizeof(answer));
                    offset += sizeof(answer);
                    (void)sendto(fd, (void*)response, offset, 0, (struct sockaddr*)&client, client_len);
                }
            },
            "voicelife_dns", 3072, &dns_socket, 5, nullptr);
    }

    // 4. 启动 httpd：GET / 返回配置页；POST /api/config 接收凭据。
    httpd_config_t http_config = HTTPD_DEFAULT_CONFIG();
    http_config.stack_size = 8192;
    httpd_handle_t server = nullptr;

    httpd_uri_t uri_root = {};
    uri_root.method = HTTP_GET;
    uri_root.uri = "/";
    uri_root.handler = &HandleHotspotIndex;

    httpd_uri_t uri_api = {};
    uri_api.method = HTTP_POST;
    uri_api.uri = "/api/config";
    uri_api.handler = &HandleHotspotConfig;

    if (httpd_start(&server, &http_config) != ESP_OK) {
        if (dns_socket >= 0) close(dns_socket);
        return Status::Error(ErrorCode::kUnavailable, "启动 HTTP 配网服务器失败");
    }
    httpd_register_uri_handler(server, &uri_root);
    httpd_register_uri_handler(server, &uri_api);

    // 5. 等待用户配置（超时或已提交）。
    const TickType_t ticks = timeout_ms == 0 ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    const TickType_t start_tick = xTaskGetTickCount();
    while (!g_hotspot_configured) {
        vTaskDelay(pdMS_TO_TICKS(200));
        if (timeout_ms != 0 && (xTaskGetTickCount() - start_tick) >= ticks) {
            break;
        }
    }
    result.configured = g_hotspot_configured;
    if (g_hotspot_configured) {
        result.ssid = g_hotspot_ssid;
        result.password = g_hotspot_password;
        ESP_LOGI(kTag, "HOTSPOT_PROVISIONED ssid=%s", g_hotspot_ssid.c_str());
    } else {
        ESP_LOGW(kTag, "HOTSPOT_PROVISION_TIMEOUT");
    }

    // 6. 清理：停止 httpd、关 DNS、停 AP。
    if (server != nullptr) httpd_stop(server);
    if (dns_socket >= 0) close(dns_socket);
    (void)esp_wifi_stop();
    if (original_mode != WIFI_MODE_NULL) {
        (void)esp_wifi_set_mode(original_mode);
    }

    return g_hotspot_configured ? Status::Ok() : Status::Error(ErrorCode::kUnavailable, "热点配网超时未配置");
}

// GET /：扫描 AP 并渲染配置页。
esp_err_t HandleHotspotIndex(httpd_req_t* req) {
    std::string list;
    wifi_scan_config_t scan_config = {};
    if (esp_wifi_scan_start(&scan_config, true) == ESP_OK) {
        uint16_t count = 0;
        (void)esp_wifi_scan_get_ap_num(&count);
        std::vector<wifi_ap_record_t> aps(count);
        if (count > 0 && esp_wifi_scan_get_ap_records(&count, aps.data()) == ESP_OK) {
            for (const auto& ap : aps) {
                const size_t ssid_len = strnlen(reinterpret_cast<const char*>(ap.ssid), sizeof(ap.ssid));
                if (ssid_len == 0) continue;
                const std::string escaped =
                    HtmlEscape(std::string_view(reinterpret_cast<const char*>(ap.ssid), ssid_len));
                list += "<li><label><input type=\"radio\" name=\"ssid\" value=\"" + escaped + "\"> " + escaped +
                        " (RSSI " + std::to_string(ap.rssi) + ")</label></li>";
            }
        }
    }
    if (list.empty()) list = "<li>未扫描到 Wi-Fi，请刷新</li>";
    std::string html = kIndexHtml;
    const size_t pos = html.find("__SSID_LIST__");
    if (pos != std::string::npos) html.replace(pos, std::string("__SSID_LIST__").size(), list);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, html.c_str(), html.size());
    return ESP_OK;
}

// POST /api/config：解析表单并写 NVS（与现有串口配网同命名空间）。
esp_err_t HandleHotspotConfig(httpd_req_t* req) {
    std::string body;
    body.resize(req->content_len);
    size_t received = 0;
    while (received < body.size()) {
        const int read = httpd_req_recv(req, &body[received], body.size() - received);
        if (read <= 0) break;
        received += static_cast<size_t>(read);
    }
    std::string ssid, password;
    if (ParseFormValue(body.c_str(), received, "ssid", ssid) &&
        ParseFormValue(body.c_str(), received, "password", password) && !ssid.empty() && ssid.size() <= kMaxSsidLen &&
        !password.empty() && password.size() <= kMaxPasswordLen) {
        nvs_handle_t handle = 0;
        if (nvs_open_from_partition("linx_secrets", kWifiNamespace, NVS_READWRITE, &handle) == ESP_OK) {
            (void)nvs_set_str(handle, kWifiSsidKey, ssid.c_str());
            (void)nvs_set_str(handle, kWifiPasswordKey, password.c_str());
            (void)nvs_commit(handle);
            nvs_close(handle);
            g_hotspot_configured = true;
            g_hotspot_ssid = ssid;
            g_hotspot_password = password;
            httpd_resp_set_type(req, "text/plain; charset=utf-8");
            httpd_resp_sendstr(req, "OK");
            return ESP_OK;
        }
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS write failed");
        return ESP_FAIL;
    }
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad request");
    return ESP_FAIL;
}

}  // namespace voicelife::runtime

#endif  // ESP_PLATFORM
