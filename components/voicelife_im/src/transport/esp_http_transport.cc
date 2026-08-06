#include "esp_http_transport.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "voicelife/im/im_endpoint.h"

namespace voicelife::im {
namespace {

constexpr char kTag[] = "voicelife_im";
constexpr int kTransportTimeoutMs = 10 * 1000;

// 读取响应体：受理结果（如 NotificationSubmission）需透传给调用方提取动作窗口。
// content_length 未知（-1，分块编码）时持续读到 EOF；否则按 Content-Length 精确读取。
void ReadResponseBody(esp_http_client_handle_t client, std::string& body) {
    const int64_t content_length = esp_http_client_get_content_length(client);
    char buffer[256];
    int64_t remaining = content_length;
    while (remaining != 0) {
        const size_t want =
            remaining > 0 ? std::min<size_t>(sizeof(buffer), static_cast<size_t>(remaining)) : sizeof(buffer);
        const int n = esp_http_client_read(client, buffer, static_cast<int>(want));
        if (n <= 0) {
            break;
        }
        body.append(buffer, static_cast<size_t>(n));
        if (remaining > 0) {
            remaining -= n;
        }
    }
}

}  // namespace

EspHttpTransport::EspHttpTransport(std::string base_url) : base_url_(std::move(base_url)) {}

ImHttpResponse EspHttpTransport::Post(const ImHttpRequest& request) {
    ImHttpResponse result;
    if (!IsHttpsGatewayUrl(base_url_)) {
        result.status = ImTransportStatus::kInvalidConfig;
        result.message = "网关地址必须使用 https:// 且不含 query/fragment";
        return result;
    }

    std::string url = base_url_;
    if (!url.empty() && url.back() == '/' && !request.path.empty() && request.path.front() == '/') {
        url.pop_back();
    }
    url += request.path;

    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.method = HTTP_METHOD_POST;
    config.timeout_ms = kTransportTimeoutMs;
    config.buffer_size_tx = request.body.size() + 32;
    config.disable_auto_redirect = true;
    // 通过系统证书 bundle 校验网关证书；若网关使用私有 CA，可改用 config.cert_pem 注入根证书。
    config.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        result.status = ImTransportStatus::kNetworkFailure;
        result.message = "esp_http_client_init 失败";
        return result;
    }

    for (const ImHttpHeader& header : request.headers) {
        const esp_err_t err = esp_http_client_set_header(client, header.name.c_str(), header.value.c_str());
        if (err != ESP_OK) {
            ESP_LOGW(kTag, "设置请求头 %s 失败：%s", header.name.c_str(), esp_err_to_name(err));
            result.status = ImTransportStatus::kNetworkFailure;
            result.message = esp_err_to_name(err);
            esp_http_client_cleanup(client);
            return result;
        }
    }
    if (!request.body.empty()) {
        const esp_err_t err = esp_http_client_set_post_field(client, request.body.data(), request.body.size());
        if (err != ESP_OK) {
            ESP_LOGW(kTag, "设置请求体失败：%s", esp_err_to_name(err));
            result.status = ImTransportStatus::kNetworkFailure;
            result.message = esp_err_to_name(err);
            esp_http_client_cleanup(client);
            return result;
        }
    }

    const esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "HTTPS 提交失败：%s", esp_err_to_name(err));
        result.status = ImTransportStatus::kNetworkFailure;
        result.message = esp_err_to_name(err);
        esp_http_client_cleanup(client);
        return result;
    }

    result.status_code = esp_http_client_get_status_code(client);
    result.message = std::to_string(result.status_code);
    if (result.status_code >= 200 && result.status_code < 300) {
        result.status = ImTransportStatus::kSuccess;
        ReadResponseBody(client, result.body);
    } else if (result.status_code == 401 || result.status_code == 403) {
        result.status = ImTransportStatus::kCredentialRejected;
    } else {
        result.status = ImTransportStatus::kHttpError;
    }
    esp_http_client_cleanup(client);
    return result;
}

}  // namespace voicelife::im
