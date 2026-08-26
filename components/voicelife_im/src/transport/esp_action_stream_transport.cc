#include "esp_action_stream_transport.h"

#include <chrono>
#include <cstdint>
#include <utility>

#include "../im_wire.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "voicelife/contracts/json.h"
#include "voicelife/im/im_endpoint.h"

namespace voicelife::im {
namespace {

constexpr char kTag[] = "voicelife_im_sse";
// 单次读取超时必须大于网关心跳间隔（20 秒），否则空闲心跳期间的读取会超时。
constexpr int kSseTimeoutMs = 30 * 1000;
// 心跳只能证明连接仍可读，不能证明动作已经送达；长时间未收到命令时主动
// 重建连接，让网关按 processing 状态重放动作，避免单条 SSE 永久卡住窗口。
constexpr int64_t kSseReconnectWithoutCommandUs = 25LL * 1000 * 1000;

int64_t MonotonicNowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
// esp_http_client_read() may wait until the requested size is available. Keep
// this smaller than a typical trailing SSE fragment so a complete action frame
// is delivered without waiting for another heartbeat.
constexpr int kSseReadBufferSize = 64;
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
    // esp_http_client_get_header() 读取的是请求头；响应头查询需要额外打开保存响应头的
    // 配置，设备端不依赖它。HTTP 状态已确认为 2xx，交给 SSE 解码器判断实际字节流，
    // 这样代理改写 Content-Type 时不会误关闭合法动作流，错误体也会按协议错误处理。
    decoder_.Reset();
    pending_.clear();
    received_action_event_ = false;
    opened_at_us_ = MonotonicNowUs();
    open_ = true;
    ESP_LOGI(kTag, "IM_ACTION_STREAM_OPENED=1 reminder_trigger_id=%s resumed=%d", reminder_trigger_id_.c_str(),
             last_event_id.empty() ? 0 : 1);
    return true;
}

StreamRead EspActionStreamTransport::Next() {
    if (!open_ || client_ == nullptr) {
        return {StreamReadStatus::kNetworkError, {}};
    }
    while (true) {
        // 优先消费上次读取已解码但未取走的帧。
        while (!pending_.empty()) {
            SseFrame frame = std::move(pending_.front());
            pending_.pop_front();
            if (frame.event != kActionEventType) {
                // 心跳等非动作事件：跳过，不属于协议错误。
                continue;
            }
            voicelife::JsonValue root;
            if (!voicelife::ParseJson(frame.data, root).ok()) {
                // 动作事件载荷损坏属于协议错误：关闭连接，交由调用方按可重连处理。
                ESP_LOGW(kTag, "动作命令载荷不是合法 JSON");
                CloseConnection();
                return {StreamReadStatus::kProtocolError, {}};
            }
            contracts::im::ReminderActionCommand command;
            if (!ParseReminderActionCommand(root, command).ok()) {
                ESP_LOGW(kTag, "动作命令载荷未通过契约校验");
                CloseConnection();
                return {StreamReadStatus::kProtocolError, {}};
            }
            // 帧 id 必须与命令 commandId 一致，防止网关错序或串帧。
            if (command.commandId != frame.id) {
                ESP_LOGW(kTag, "动作命令 frame.id 与 commandId 不一致");
                CloseConnection();
                return {StreamReadStatus::kProtocolError, {}};
            }
            received_action_event_ = true;
            ESP_LOGI(kTag,
                     "IM_ACTION_COMMAND_RECEIVED=1 command_id=%s operation_id=%s reminder_trigger_id=%s action=%s",
                     command.commandId.c_str(), command.operationId.c_str(), command.reminderTriggerId.c_str(),
                     command.action.c_str());
            return {StreamReadStatus::kCommand, command};
        }
        if (!received_action_event_ && decoder_.BufferedBytes() == 0 && opened_at_us_ > 0 &&
            MonotonicNowUs() - opened_at_us_ >= kSseReconnectWithoutCommandUs) {
            ESP_LOGW(kTag, "动作流超过 25 秒未收到命令且没有未完成帧，主动重连");
            CloseConnection();
            return {StreamReadStatus::kNetworkError, {}};
        }
        char buffer[kSseReadBufferSize];
        const int n = esp_http_client_read(client_, buffer, sizeof(buffer));
        if (n < 0) {
            // 读取返回负数为网络/TLS 错误，与正常流结束区分，调用方应重连。
            ESP_LOGW(kTag, "动作流读取网络错误（%d）", n);
            CloseConnection();
            return {StreamReadStatus::kNetworkError, {}};
        }
        if (n == 0) {
            // ESP-IDF 会在 chunked 响应收到 FIN 时调用 parser 的 message_complete，
            // 即使连接并未收到完整的服务端结束帧，is_complete_data_received() 也可能为真。
            // 在本次连接尚未收到任何动作命令前，EOF 一律按断线处理，保留窗口并重连，
            // 避免动作发布与设备建流/读流的竞态丢失窗口。
            if (!received_action_event_) {
                ESP_LOGW(kTag, "动作流在收到命令前结束，准备重连");
                CloseConnection();
                return {StreamReadStatus::kNetworkError, {}};
            }
            // SSE 使用 chunked 响应时，只有解析到完整的 0 长度 chunk 才是服务端
            // 主动结束。代理或 Wi-Fi 提前 FIN 会同样返回 0，但必须按断线重连，
            // 否则运行时会误把尚未完成的动作窗口标记为正常结束并丢弃它。
            if (!esp_http_client_is_complete_data_received(client_)) {
                ESP_LOGW(kTag, "动作流在响应完成前断开，准备重连");
                CloseConnection();
                return {StreamReadStatus::kNetworkError, {}};
            }
            // 服务端正常关闭连接，流结束。
            ESP_LOGW(kTag, "动作流正常结束");
            CloseConnection();
            return {StreamReadStatus::kEndOfStream, {}};
        }
        // 解码器输出固定为 vector；解码后整体移交 deque，保持待消费队列 O(1) 出队。
        std::vector<SseFrame> frames;
        decoder_.Feed(std::string_view(buffer, n), frames);
        ESP_LOGI(kTag, "IM_ACTION_STREAM_READ bytes=%d frames=%u pending=%u buffered=%u", n,
                 static_cast<unsigned>(frames.size()), static_cast<unsigned>(pending_.size()),
                 static_cast<unsigned>(decoder_.BufferedBytes()));
        for (SseFrame& frame : frames) {
            pending_.push_back(std::move(frame));
        }
        // 单帧超限视为协议错误：关闭连接，交由调用方按可重连处理。
        if (decoder_.Overflowed()) {
            ESP_LOGE(kTag, "动作流单帧超过上限，中止连接");
            CloseConnection();
            return {StreamReadStatus::kProtocolError, {}};
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
    opened_at_us_ = 0;
}

}  // namespace voicelife::im
