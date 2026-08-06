#include "esp_http_transport.h"

#include <cstddef>
#include <string>
#include <utility>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"

namespace voicelife::im {
namespace {

constexpr char kTag[] = "voicelife_im";
constexpr int kTransportTimeoutMs = 10 * 1000;

}  // namespace

EspHttpTransport::EspHttpTransport(std::string base_url) : base_url_(std::move(base_url)) {}

ImHttpResponse EspHttpTransport::Post(const ImHttpRequest& request) {
    ImHttpResponse result;

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
    // 通过系统证书 bundle 校验网关证书；若网关使用私有 CA，可改用 config.cert_pem 注入根证书。
    config.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        result.status = ImTransportStatus::kNetworkFailure;
        result.message = "esp_http_client_init 失败";
        return result;
    }

    for (const ImHttpHeader& header : request.headers) {
        esp_http_client_set_header(client, header.name.c_str(), header.value.c_str());
    }
    if (!request.body.empty()) {
        esp_http_client_set_post_field(client, request.body.data(), request.body.size());
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
    } else if (result.status_code == 401 || result.status_code == 403) {
        result.status = ImTransportStatus::kCredentialRejected;
    } else {
        result.status = ImTransportStatus::kHttpError;
    }
    esp_http_client_cleanup(client);
    return result;
}

}  // namespace voicelife::im
