#include "esp_action_stream_transport.h"

#include <string_view>
#include <utility>

#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "voicelife/contracts/json.h"
#include "voicelife/im/im_endpoint.h"

namespace voicelife::im {
namespace {

constexpr char kTag[] = "voicelife_im";
// 单次读取超时必须大于网关心跳间隔（20 秒），否则空闲心跳期间的读取会超时。
constexpr int kSseTimeoutMs = 30 * 1000;
constexpr int kSseReadBufferSize = 256;
constexpr const char* kActionStreamPrefix = "/v1/devices/";
constexpr const char* kActionStreamSuffix = "/reminder-actions/stream";
constexpr const char* kActionEventType = "reminder.action";

}  // namespace

EspActionStreamTransport::EspActionStreamTransport(std::string base_url, ImCredentialProvider& credentials,
                                                   std::string reminder_trigger_id)
    : base_url_(std::move(base_url)), credentials_(credentials), reminder_trigger_id_(std::move(reminder_trigger_id)) {}

void EspActionStreamTransport::Open(const std::string& last_event_id) {
    CloseConnection();
    if (!IsHttpsGatewayUrl(base_url_)) {
        ESP_LOGE(kTag, "动作流网关地址必须使用 https:// 且不含 query/fragment");
        return;
    }

    std::string url = base_url_;
    if (!url.empty() && url.back() == '/') {
        url.pop_back();
    }
    url += kActionStreamPrefix + credentials_.DeviceId() + kActionStreamSuffix;
    url += "?reminderType=strong&reminderTriggerId=" + reminder_trigger_id_;
    if (!last_event_id.empty()) {
        url += "&lastEventId=" + last_event_id;
    }

    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.method = HTTP_METHOD_GET;
    config.timeout_ms = kSseTimeoutMs;
    config.buffer_size = kSseReadBufferSize;
    config.disable_auto_redirect = true;
    // 与 HTTPS 上报一致，通过系统证书 bundle 校验网关证书。
    config.crt_bundle_attach = esp_crt_bundle_attach;

    client_ = esp_http_client_init(&config);
    if (client_ == nullptr) {
        ESP_LOGE(kTag, "esp_http_client_init 失败");
        return;
    }
    const std::string bearer = "Bearer " + credentials_.DeviceToken();
    esp_http_client_set_header(client_, "Authorization", bearer.c_str());
    esp_http_client_set_header(client_, "Accept", "text/event-stream");
    if (esp_http_client_open(client_, 0) != ESP_OK || esp_http_client_fetch_headers(client_) < 0) {
        ESP_LOGW(kTag, "动作流连接失败");
        CloseConnection();
        return;
    }
    const int status = esp_http_client_get_status_code(client_);
    if (status < 200 || status >= 300) {
        ESP_LOGW(kTag, "动作流连接被拒：HTTP %d", status);
        CloseConnection();
        return;
    }
    decoder_.Reset();
    pending_.clear();
    open_ = true;
}

std::optional<contracts::im::ReminderActionCommand> EspActionStreamTransport::Next() {
    if (!open_ || client_ == nullptr) {
        return std::nullopt;
    }
    while (true) {
        // 优先消费上次读取已解码但未取走的帧。
        while (!pending_.empty()) {
            SseFrame frame = std::move(pending_.front());
            pending_.erase(pending_.begin());
            if (frame.event != kActionEventType) {
                continue;
            }
            voicelife::JsonValue root;
            if (!voicelife::ParseJson(frame.data, root).ok()) {
                continue;
            }
            contracts::im::ReminderActionCommand command;
            if (ParseReminderActionCommand(root, command).ok()) {
                return command;
            }
        }
        char buffer[kSseReadBufferSize];
        const int n = esp_http_client_read(client_, buffer, sizeof(buffer));
        if (n <= 0) {
            // 服务端关闭连接或读取失败，流结束。
            ESP_LOGW(kTag, "动作流读取结束（%d）", n);
            CloseConnection();
            return std::nullopt;
        }
        decoder_.Feed(std::string_view(buffer, n), pending_);
    }
}

void EspActionStreamTransport::Close() { CloseConnection(); }

void EspActionStreamTransport::CloseConnection() {
    if (client_ != nullptr) {
        esp_http_client_close(client_);
        esp_http_client_cleanup(client_);
        client_ = nullptr;
    }
    open_ = false;
}

}  // namespace voicelife::im
