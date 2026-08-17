#include "esp_http_transport.h"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "im_response_reader.h"
#include "voicelife/im/esp_http_transport_factory.h"
#include "voicelife/im/im_endpoint.h"
#include "voicelife/im/im_http_policy.h"

namespace voicelife::im {
namespace {

constexpr char kTag[] = "voicelife_im_http";
constexpr size_t kMinimumTransmitBufferBytes = 1024;
// 受理结果响应体上限：防止恶意网关回灌无界响应耗尽设备堆内存。
constexpr size_t kMaxResponseBodyBytes = 64 * 1024;

void LogHttpHeap(std::string_view phase) {
    ESP_LOGI(kTag, "IM_HTTP_HEAP phase=%.*s internal_free=%u internal_largest=%u psram_free=%u",
             static_cast<int>(phase.size()), phase.data(),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
}

/// 把 esp_http_client 适配为 ImResponseReader，供 ReadResponseBody 判定读取完整性。
class EspResponseReader : public ImResponseReader {
   public:
    explicit EspResponseReader(esp_http_client_handle_t client) : client_(client) {}
    int64_t ContentLength() const override { return esp_http_client_get_content_length(client_); }
    int Read(char* buffer, size_t size) override {
        return esp_http_client_read(client_, buffer, static_cast<int>(size));
    }

   private:
    esp_http_client_handle_t client_;
};

}  // namespace

EspHttpTransport::EspHttpTransport(std::string base_url) : base_url_(std::move(base_url)) {}

std::unique_ptr<ImTransport> CreateEspHttpTransport(std::string gateway_origin) {
    return std::make_unique<EspHttpTransport>(std::move(gateway_origin));
}

ImHttpResponse EspHttpTransport::Post(const ImHttpRequest& request) { return Perform(request, HTTP_METHOD_POST); }

ImHttpResponse EspHttpTransport::Get(const ImHttpRequest& request) { return Perform(request, HTTP_METHOD_GET); }

ImHttpResponse EspHttpTransport::Perform(const ImHttpRequest& request, esp_http_client_method_t method) {
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
    config.method = method;
    config.timeout_ms = static_cast<int>(kImHttpRequestTimeoutMs);
    // GET 没有 body，但仍需容纳 URL、Bearer 头与 esp_http_client 生成的请求头。
    // 保留固定下限，POST 则按受控请求体继续扩展。
    config.buffer_size_tx = std::max(kMinimumTransmitBufferBytes, request.body.size() + 32);
    config.disable_auto_redirect = true;
    // Authorization 由调用方以 Bearer 头提供。禁止 esp_http_client 在 401 时尝试
    // Basic/Digest 自动认证，否则它会把合法的 WWW-Authenticate: Bearer
    // 转换成 ESP_ERR_NOT_SUPPORTED，掩盖真实凭据拒绝状态。
    config.max_authorization_retries = -1;
    // 通过系统证书 bundle 校验网关证书；若网关使用私有 CA，可改用 config.cert_pem 注入根证书。
    config.crt_bundle_attach = esp_crt_bundle_attach;

    LogHttpHeap("init");
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
    if (method == HTTP_METHOD_POST && !request.body.empty()) {
        const esp_err_t err = esp_http_client_set_post_field(client, request.body.data(), request.body.size());
        if (err != ESP_OK) {
            ESP_LOGW(kTag, "设置请求体失败：%s", esp_err_to_name(err));
            result.status = ImTransportStatus::kNetworkFailure;
            result.message = esp_err_to_name(err);
            esp_http_client_cleanup(client);
            return result;
        }
    }

    // 用 open → write → fetch_headers → read 序列读取响应体，而不是 perform()：
    // perform() 会在 HTTP_EVENT_ON_DATA 状态下把 2xx 响应体内部读取并丢弃，
    // 之后 esp_http_client_read() 读不到 body（#241 真机配对 201 响应被整段丢弃）。
    // open() 发送请求行与请求头并建立连接，write() 发送 POST 请求体，
    // fetch_headers() 只消费响应头（响应体仍留在传输层，由 read() 逐块取回）。
    const esp_err_t open_err = esp_http_client_open(client, static_cast<int>(request.body.size()));
    if (open_err != ESP_OK) {
        LogHttpHeap("open_failed");
        result.status_code = esp_http_client_get_status_code(client);
        if (result.status_code == 401 || result.status_code == 403) {
            result.status = ImTransportStatus::kCredentialRejected;
            result.message = std::to_string(result.status_code);
            esp_http_client_cleanup(client);
            return result;
        }
        ESP_LOGW(kTag, "HTTPS 连接/请求头提交失败：%s", esp_err_to_name(open_err));
        result.status = ImTransportStatus::kNetworkFailure;
        result.message = esp_err_to_name(open_err);
        esp_http_client_cleanup(client);
        return result;
    }
    if (method == HTTP_METHOD_POST && !request.body.empty()) {
        const int written = esp_http_client_write(client, request.body.data(), static_cast<int>(request.body.size()));
        if (written < 0 || static_cast<size_t>(written) != request.body.size()) {
            ESP_LOGW(kTag, "POST 请求体提交失败：写入 %d/%u 字节", written, static_cast<unsigned>(request.body.size()));
            result.status = ImTransportStatus::kNetworkFailure;
            result.message = "POST 请求体提交失败";
            esp_http_client_cleanup(client);
            return result;
        }
    }
    const int content_length = esp_http_client_fetch_headers(client);
    if (content_length < 0) {
        ESP_LOGW(kTag, "HTTP 头解析失败：%d", content_length);
        result.status = ImTransportStatus::kNetworkFailure;
        result.message = "HTTP 头解析失败";
        esp_http_client_cleanup(client);
        return result;
    }
    result.status_code = esp_http_client_get_status_code(client);
    result.message = std::to_string(result.status_code);
    if (result.status_code == 401 || result.status_code == 403) {
        result.status = ImTransportStatus::kCredentialRejected;
        esp_http_client_cleanup(client);
        return result;
    }
    if (result.status_code >= 200 && result.status_code < 300) {
        result.status = ImTransportStatus::kSuccess;
    } else {
        result.status = ImTransportStatus::kHttpError;
    }

    EspResponseReader reader(client);
    const bool complete_body = ReadResponseBody(reader, result.body, kMaxResponseBodyBytes);
    // 响应体提前 EOF、读取错误或超限截断都不得按成功受理处理：
    // 不完整的 NotificationSubmission 无法提取可靠动作窗口，
    // 按未确认处理由调用方重连重放。
    if (result.status == ImTransportStatus::kSuccess && !complete_body) {
        ESP_LOGW(kTag, "受理结果响应不完整（读取错误或超过 %zu 字节上限），按未受理处理", kMaxResponseBodyBytes);
        result.status = ImTransportStatus::kNetworkFailure;
        result.message = "受理结果响应不完整";
    }
    if (!complete_body && result.status != ImTransportStatus::kNetworkFailure) result.body.clear();
    esp_http_client_cleanup(client);
    return result;
}

}  // namespace voicelife::im
