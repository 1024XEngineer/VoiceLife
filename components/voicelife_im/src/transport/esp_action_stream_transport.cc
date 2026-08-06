#include "esp_action_stream_transport.h"

#include <string_view>
#include <utility>

#include "../im_wire.h"
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
constexpr const char* kSseContentType = "text/event-stream";

}  // namespace

EspActionStreamTransport::EspActionStreamTransport(std::string base_url, ImCredentialProvider& credentials,
                                                   std::string reminder_trigger_id)
    : base_url_(std::move(base_url)), credentials_(credentials), reminder_trigger_id_(std::move(reminder_trigger_id)) {}

bool EspActionStreamTransport::Open(const std::string& last_event_id) {
    CloseConnection();
    if (!IsHttpsGatewayUrl(base_url_)) {
        ESP_LOGE(kTag, "动作流网关地址必须使用 https:// 且不含 query/fragment");
        return false;
    }

    std::string url = base_url_;
    if (!url.empty() && url.back() == '/') {
        url.pop_back();
    }
    // deviceId/reminderTriggerId 可能含 / ? & # 等字符，按 path/query 段百分号编码，
    // 防止改写请求路径或注入额外参数。
    url += kActionStreamPrefix + EncodePathSegment(credentials_.DeviceId()) + kActionStreamSuffix;
    url += "?reminderType=strong&reminderTriggerId=" + EncodePathSegment(reminder_trigger_id_);

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
        return false;
    }
    const std::string bearer = "Bearer " + credentials_.DeviceToken();
    esp_http_client_set_header(client_, "Authorization", bearer.c_str());
    esp_http_client_set_header(client_, "Accept", kSseContentType);
    if (!last_event_id.empty()) {
        // Last-Event-ID 是请求头而非 query 参数，且值原样传递（不得百分号编码）。
        esp_http_client_set_header(client_, "Last-Event-ID", last_event_id.c_str());
    }
    if (esp_http_client_open(client_, 0) != ESP_OK || esp_http_client_fetch_headers(client_) < 0) {
        ESP_LOGW(kTag, "动作流连接失败");
        CloseConnection();
        return false;
    }
    const int status = esp_http_client_get_status_code(client_);
    if (status < 200 || status >= 300) {
        ESP_LOGW(kTag, "动作流连接被拒：HTTP %d", status);
        CloseConnection();
        return false;
    }
    // 校验响应类型确为 SSE；网关误回 JSON 错误体时提前识别，避免把错误体当流解析。
    char* content_type = nullptr;
    if (esp_http_client_get_header(client_, "Content-Type", &content_type) != ESP_OK || content_type == nullptr ||
        std::string_view(content_type).find(kSseContentType) == std::string_view::npos) {
        ESP_LOGW(kTag, "动作流响应 Content-Type 非 %s", kSseContentType);
        CloseConnection();
        return false;
    }
    decoder_.Reset();
    pending_.clear();
    open_ = true;
    return true;
}

std::optional<contracts::im::ReminderActionCommand> EspActionStreamTransport::Next() {
    if (!open_ || client_ == nullptr) {
        return std::nullopt;
    }
    while (true) {
        // 优先消费上次读取已解码但未取走的帧。
        while (!pending_.empty()) {
            SseFrame frame = std::move(pending_.front());
            pending_.pop_front();
            if (frame.event != kActionEventType) {
                continue;
            }
            voicelife::JsonValue root;
            if (!voicelife::ParseJson(frame.data, root).ok()) {
                ESP_LOGW(kTag, "动作命令载荷不是合法 JSON");
                continue;
            }
            contracts::im::ReminderActionCommand command;
            if (!ParseReminderActionCommand(root, command).ok()) {
                ESP_LOGW(kTag, "动作命令载荷未通过契约校验");
                continue;
            }
            // 帧 id 必须与命令 commandId 一致，防止网关错序或串帧。
            if (command.commandId != frame.id) {
                ESP_LOGW(kTag, "动作命令 frame.id 与 commandId 不一致");
                continue;
            }
            return command;
        }
        char buffer[kSseReadBufferSize];
        const int n = esp_http_client_read(client_, buffer, sizeof(buffer));
        if (n <= 0) {
            // 服务端关闭连接或读取失败，流结束。
            ESP_LOGW(kTag, "动作流读取结束（%d）", n);
            CloseConnection();
            return std::nullopt;
        }
        // 解码器输出固定为 vector；解码后整体移交 deque，保持待消费队列 O(1) 出队。
        std::vector<SseFrame> frames;
        decoder_.Feed(std::string_view(buffer, n), frames);
        for (SseFrame& frame : frames) {
            pending_.push_back(std::move(frame));
        }
        // 单帧超限视为协议错误：关闭连接，交由调用方按可重连处理。
        if (decoder_.Overflowed()) {
            ESP_LOGE(kTag, "动作流单帧超过上限，中止连接");
            CloseConnection();
            return std::nullopt;
        }
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
