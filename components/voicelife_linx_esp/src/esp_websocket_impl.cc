#include "esp_websocket_impl.h"

#include <climits>
#include <string>
#include <string_view>
#include <utility>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

namespace voicelife::linx_esp {
namespace {

Status EspError(const char* operation, esp_err_t error) {
    return Status::Error(ErrorCode::kUnavailable, std::string(operation) + " 失败，esp_err_t=" + std::to_string(error));
}

}  // namespace

Status EspWebSocketTransport::Impl::Connect(const linx::LinxConnectionConfig& config, linx::LinxTransportSink sink) {
    std::unique_lock<std::recursive_mutex> lifecycle_lock(lifecycle_mutex_);
    const bool secure = config.websocket_url.rfind("wss://", 0) == 0;
    const bool explicitly_allowed_insecure = options_.allow_insecure_ws && config.websocket_url.rfind("ws://", 0) == 0;
    if (!config.valid() || (!secure && !explicitly_allowed_insecure) || options_.event_queue_capacity == 0 ||
        options_.event_chunk_bytes == 0 || options_.event_chunk_bytes > detail::kMaxEventChunkBytes ||
        options_.max_message_bytes == 0 || options_.websocket_task_stack_size == 0 ||
        options_.worker_task_stack_size == 0) {
        return Status::Error(ErrorCode::kInvalidArgument, "ESP Linx WSS 配置无效");
    }
    if (closing_.load() && state_ != TransportState::kFailed) {
        return Status::Error(ErrorCode::kConflict, "ESP Linx Transport 正在关闭");
    }
    if (state_ == TransportState::kConnecting || state_ == TransportState::kConnected ||
        state_ == TransportState::kReconnecting) {
        return Status::Error(ErrorCode::kConflict, "ESP Linx Transport 已连接");
    }
    if (state_ == TransportState::kFailed) {
        lifecycle_lock.unlock();
        Close();
        lifecycle_lock.lock();
    }

    auto token = secrets_.Resolve(config.token_ref);
    if (!token.ok() || !token.value.has_value() || token.value->empty()) {
        return token.ok() ? Status::Error(ErrorCode::kInvalidArgument, "Linx token 为空") : token.status;
    }
    const auto headers = BuildHeaders(config, *token.value);
    if (!headers.ok() || !headers.value.has_value()) {
        return headers.status;
    }
    headers_ = *headers.value;
    state_ = TransportState::kConnecting;
    closing_.store(false);
    {
        std::lock_guard<std::mutex> callback_lock(callback_mutex_);
        accepting_events_.store(true);
        sink_ = std::move(sink);
    }
    {
        std::lock_guard<std::mutex> status_lock(status_mutex_);
        error_status_ = Status::Ok();
    }
    if (!PrepareWorker()) {
        state_ = TransportState::kFailed;
        return Status::Error(ErrorCode::kInternal, "ESP Linx 事件队列创建失败");
    }

    esp_websocket_client_config_t websocket_config = {};
    websocket_config.uri = config.websocket_url.c_str();
    websocket_config.headers = headers_.c_str();
    websocket_config.disable_auto_reconnect = false;
    websocket_config.enable_close_reconnect = options_.enable_close_reconnect;
    websocket_config.reconnect_timeout_ms = options_.reconnect_timeout_ms;
    websocket_config.network_timeout_ms = options_.network_timeout_ms;
    websocket_config.task_stack = static_cast<int>(options_.websocket_task_stack_size);
    websocket_config.buffer_size = static_cast<int>(options_.event_chunk_bytes);
    websocket_config.crt_bundle_attach = secure ? esp_crt_bundle_attach : nullptr;
    websocket_config.skip_cert_common_name_check = false;
    websocket_config.user_context = this;

    client_ = esp_websocket_client_init(&websocket_config);
    if (client_ == nullptr) {
        lifecycle_lock.unlock();
        Close();
        lifecycle_lock.lock();
        state_ = TransportState::kFailed;
        return Status::Error(ErrorCode::kInternal, "ESP WebSocket client 初始化失败");
    }
    esp_err_t status = esp_websocket_register_events(client_, WEBSOCKET_EVENT_ANY, &OnEvent, this);
    if (status != ESP_OK) {
        lifecycle_lock.unlock();
        Close();
        lifecycle_lock.lock();
        state_ = TransportState::kFailed;
        return EspError("注册 WebSocket 事件", status);
    }
    status = esp_websocket_client_start(client_);
    if (status != ESP_OK) {
        lifecycle_lock.unlock();
        Close();
        lifecycle_lock.lock();
        state_ = TransportState::kFailed;
        return EspError("启动 WebSocket client", status);
    }

    // The worker invokes on_connected(), which may immediately send the
    // hello frame through this transport. Do not hold the lifecycle lock
    // while waiting for that callback.
    connect_waiting_.store(true);
    lifecycle_lock.unlock();
    const EventBits_t bits = xEventGroupWaitBits(state_events_, detail::kConnectedBit | detail::kFailedBit, pdTRUE,
                                                 pdFALSE, pdMS_TO_TICKS(options_.connect_timeout_ms));
    lifecycle_lock.lock();
    connect_waiting_.store(false);
    if ((bits & detail::kConnectedBit) != 0 && !closing_.load() && state_ == TransportState::kConnected) {
        return Status::Ok();
    }
    Status failure;
    {
        std::lock_guard<std::mutex> status_lock(status_mutex_);
        failure = error_status_;
    }
    lifecycle_lock.unlock();
    Close();
    lifecycle_lock.lock();
    state_ = TransportState::kFailed;
    if (!failure.ok()) {
        return failure;
    }
    return Status::Error(ErrorCode::kUnavailable, "ESP Linx WSS 连接超时");
}

Status EspWebSocketTransport::Impl::SendText(std::string_view message) {
    std::lock_guard<std::recursive_mutex> lifecycle_lock(lifecycle_mutex_);
    if (closing_.load() || client_ == nullptr || state_ != TransportState::kConnected ||
        message.size() > static_cast<size_t>(INT_MAX)) {
        return Status::Error(ErrorCode::kUnavailable, "ESP Linx Transport 尚未连接或消息过大");
    }
    const int sent = esp_websocket_client_send_text(client_, message.data(), static_cast<int>(message.size()),
                                                    pdMS_TO_TICKS(options_.network_timeout_ms));
    if (sent < 0 || static_cast<size_t>(sent) != message.size()) {
        return Status::Error(ErrorCode::kUnavailable, "发送 Linx 文本消息短写或失败");
    }
    return Status::Ok();
}

Status EspWebSocketTransport::Impl::SendAudio(const voice::AudioFrame& frame) {
    std::lock_guard<std::recursive_mutex> lifecycle_lock(lifecycle_mutex_);
    if (closing_.load() || client_ == nullptr || state_ != TransportState::kConnected || frame.payload.empty() ||
        frame.payload.size() > static_cast<size_t>(INT_MAX)) {
        return Status::Error(ErrorCode::kUnavailable, "ESP Linx Transport 尚未连接");
    }
    const int sent = esp_websocket_client_send_bin(client_, reinterpret_cast<const char*>(frame.payload.data()),
                                                   static_cast<int>(frame.payload.size()),
                                                   pdMS_TO_TICKS(options_.network_timeout_ms));
    if (sent < 0 || static_cast<size_t>(sent) != frame.payload.size()) {
        return Status::Error(ErrorCode::kUnavailable, "发送 Linx 音频短写或失败");
    }
    return Status::Ok();
}

Status EspWebSocketTransport::Impl::Close() {
    std::lock_guard<std::recursive_mutex> close_lock(close_mutex_);
    std::unique_lock<std::recursive_mutex> lifecycle_lock(lifecycle_mutex_);
    closing_.store(true);
    running_.store(false);
    state_ = TransportState::kDisconnected;
    {
        std::lock_guard<std::mutex> callback_lock(callback_mutex_);
        accepting_events_.store(false);
    }
    Status status = Status::Ok();
    lifecycle_lock.unlock();
    if (client_ != nullptr) {
        const esp_err_t stop_status = esp_websocket_client_stop(client_);
        if (stop_status != ESP_OK && stop_status != ESP_ERR_INVALID_STATE) {
            status = EspError("停止 WebSocket client", stop_status);
        }
    }
    lifecycle_lock.lock();
    if (event_queue_ != nullptr) {
        detail::EventEnvelope shutdown;
        shutdown.kind = detail::EventKind::kShutdown;
        (void)xQueueSend(event_queue_, &shutdown, pdMS_TO_TICKS(100));
    }
    const bool called_from_worker = worker_ != nullptr && xTaskGetCurrentTaskHandle() == worker_;
    if (called_from_worker) {
        return status;
    }
    if (worker_ != nullptr) {
        if (xSemaphoreTake(worker_stopped_, pdMS_TO_TICKS(1000)) != pdTRUE) {
            state_ = TransportState::kFailed;
            return status.ok() ? Status::Error(ErrorCode::kUnavailable, "等待 ESP Linx worker 退出超时") : status;
        }
        worker_ = nullptr;
    }
    if (client_ != nullptr) {
        const esp_err_t destroy_status = esp_websocket_client_destroy(client_);
        client_ = nullptr;
        if (status.ok() && destroy_status != ESP_OK) {
            status = EspError("销毁 WebSocket client", destroy_status);
        }
    }
    if (!connect_waiting_.load()) {
        CleanupWorker();
    }
    assembler_.Reset();
    {
        std::lock_guard<std::mutex> callback_lock(callback_mutex_);
        sink_ = {};
    }
    headers_.assign(headers_.size(), '\0');
    headers_.clear();
    return status;
}

void EspWebSocketTransport::Impl::SetGeneration(uint64_t generation) {
    std::lock_guard<std::recursive_mutex> lifecycle_lock(lifecycle_mutex_);
    generation_.store(generation);
    std::lock_guard<std::mutex> lock(assembler_mutex_);
    assembler_.Reset();
}

bool EspWebSocketTransport::Impl::ValidHeaderValue(std::string_view value) {
    for (const unsigned char character : value) {
        if (character < 0x20U || character == 0x7fU) {
            return false;
        }
    }
    return !value.empty();
}

Result<std::string> EspWebSocketTransport::Impl::BuildHeaders(const linx::LinxConnectionConfig& config,
                                                              std::string_view token) {
    if (!ValidHeaderValue(token) || !ValidHeaderValue(config.device_id) || !ValidHeaderValue(config.client_id)) {
        return Result<std::string>::Failure(ErrorCode::kInvalidArgument, "Linx HTTP header 值包含控制字符或为空");
    }
    std::string authorization(token);
    if (authorization.rfind("Bearer ", 0) != 0) {
        authorization.insert(0, "Bearer ");
    }
    return Result<std::string>::Success("Authorization: " + authorization + "\r\nProtocol-Version: 1\r\nDevice-Id: " +
                                        config.device_id + "\r\nClient-Id: " + config.client_id + "\r\n");
}

bool EspWebSocketTransport::Impl::PrepareWorker() {
    const size_t event_queue_bytes = options_.event_queue_capacity * sizeof(detail::EventEnvelope);
#if CONFIG_SPIRAM && (configSUPPORT_STATIC_ALLOCATION == 1)
    // The WebSocket callback runs in task context, so its bounded frame queue
    // may live in PSRAM. Keeping this 128 KiB burst buffer out of internal RAM
    // leaves enough contiguous memory for TLS and the audio pipeline.
    event_queue_ = xQueueCreateWithCaps(options_.event_queue_capacity, sizeof(detail::EventEnvelope),
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    event_queue_uses_caps_ = event_queue_ != nullptr;
    if (event_queue_ == nullptr) {
        ESP_LOGW(detail::kTag, "LINX_WS_QUEUE_PSRAM_ALLOC_FAILED bytes=%u psram_free=%u",
                 static_cast<unsigned>(event_queue_bytes),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    }
#endif
    if (event_queue_ == nullptr) {
        event_queue_ = xQueueCreate(options_.event_queue_capacity, sizeof(detail::EventEnvelope));
    }
    state_events_ = xEventGroupCreate();
    worker_stopped_ = xSemaphoreCreateBinary();
    if (event_queue_ == nullptr || state_events_ == nullptr || worker_stopped_ == nullptr) {
        ESP_LOGW(detail::kTag, "LINX_WS_QUEUE_ALLOC_FAILED bytes=%u internal_free=%u",
                 static_cast<unsigned>(event_queue_bytes),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
        CleanupWorker();
        return false;
    }
    running_.store(true);
    // Process received frames ahead of the WebSocket client task so the
    // bounded queue drains during bursty STT/TTS traffic.
    if (xTaskCreate(&WorkerEntry, "linx_ws_events", options_.worker_task_stack_size, this, 6, &worker_) != pdPASS) {
        CleanupWorker();
        return false;
    }
    return true;
}

void EspWebSocketTransport::Impl::CleanupWorker() {
    if (worker_ != nullptr) {
        return;
    }
    if (event_queue_ != nullptr) {
#if CONFIG_SPIRAM && (configSUPPORT_STATIC_ALLOCATION == 1)
        if (event_queue_uses_caps_) {
            vQueueDeleteWithCaps(event_queue_);
        } else {
            vQueueDelete(event_queue_);
        }
#else
        vQueueDelete(event_queue_);
#endif
        event_queue_ = nullptr;
        event_queue_uses_caps_ = false;
    }
    if (state_events_ != nullptr) {
        vEventGroupDelete(state_events_);
        state_events_ = nullptr;
    }
    if (worker_stopped_ != nullptr) {
        vSemaphoreDelete(worker_stopped_);
        worker_stopped_ = nullptr;
    }
}

}  // namespace voicelife::linx_esp
