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
#include "voicelife/linx_esp/linx_tx_policy.h"

namespace voicelife::linx_esp {
namespace {

Status EspError(const char* operation, esp_err_t error) {
    return Status::Error(ErrorCode::kUnavailable, std::string(operation) + " 失败，esp_err_t=" + std::to_string(error));
}

}  // namespace

EspWebSocketTransport::Impl::~Impl() {
    Close();
    // TX 任务栈（PSRAM）与 TCB（内部 RAM）常驻，随 Transport 析构释放；task 已由
    // Close()→CleanupWorker 删除，此处仅归还内存缓冲。
    if (tx_stack_ != nullptr) {
        heap_caps_free(tx_stack_);
        tx_stack_ = nullptr;
    }
    if (tx_tcb_ != nullptr) {
        heap_caps_free(tx_tcb_);
        tx_tcb_ = nullptr;
    }
}

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
        std::lock_guard<std::mutex> sink_lock(sink_mutex_);
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
    // 脱敏诊断：仅记录控制消息的 type/state 字段，不输出 token、设备 ID 或完整消息。
    const bool is_listen = message.find("\"type\":\"listen\"") != std::string_view::npos;
    const bool is_listen_boundary = SelectLinxTextTxLane(message) == LinxTextTxLane::kMediaOrdered;
    const bool is_listen_stop = is_listen_boundary && message.find("\"state\":\"stop\"") != std::string_view::npos;
    const bool is_abort = message.find("\"type\":\"abort\"") != std::string_view::npos;
    const bool is_control = (is_listen && !is_listen_boundary) || is_abort;
    // abort 可以抢占旧音频；listen.start/stop 必须与 PCM 共享媒体 FIFO，避免
    // TX worker 在 start 尚未发送时先取出首个二进制帧。
    QueueHandle_t target = is_listen_boundary ? tx_queue_ : tx_control_queue_;
    if (target == nullptr) {
        return Status::Error(ErrorCode::kUnavailable, "ESP Linx TX 队列未就绪");
    }
    auto* item = TryAcquireTxItem();
    if (item == nullptr) return Status::Error(ErrorCode::kUnavailable, "ESP Linx TX item pool 已满");
    item->kind = detail::LinxTxItem::Kind::kText;
    item->generation = generation_.load();
    item->sequence = 0;
    item->payload.assign(message.begin(), message.end());
    // 非控制文本可短暂等待 TX 队列空位，避免工具结果因慢网络而打断交互。
    // listen.stop 是实时音频的结束边界：在等待 FIFO 空位前就关闭闸门，
    // 确保 stop 入队期间不会继续接收并发送迟到的 PCM。
    // A stop is an ordered media boundary. Wait for the FIFO to drain rather
    // than evicting PCM, otherwise the server can observe a truncated stream
    // or a stop followed by a late binary frame and reset the WebSocket.
    if (is_listen_stop) {
        media_tx_open_ = false;
    }
    const TickType_t wait_ticks =
        is_listen_stop ? pdMS_TO_TICKS(options_.tx_timeout_ms) : (is_control ? 0 : pdMS_TO_TICKS(150));
    if (xQueueSend(target, &item, wait_ticks) != pdTRUE) {
        if (!is_listen_stop) {
            ReleaseTxItem(item);
            return Status::Error(ErrorCode::kUnavailable, "ESP Linx TX 队列已满");
        }
        ReleaseTxItem(item);
        return Status::Error(ErrorCode::kUnavailable, "ESP Linx 音频 TX 队列等待 stop 超时");
    }
    if (is_listen_stop) {
        ESP_LOGI(detail::kTag, "LINX_TX_STOP_QUEUE enqueued_audio=%llu dropped_audio=%llu queued_media=%u",
                 static_cast<unsigned long long>(tx_audio_enqueued_.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(tx_audio_dropped_.load(std::memory_order_relaxed)),
                 static_cast<unsigned>(uxQueueMessagesWaiting(tx_queue_)));
    }
    if (is_listen_stop || is_abort) {
        media_tx_open_ = false;
    } else if (is_listen && message.find("\"state\":\"start\"") != std::string_view::npos) {
        media_tx_open_ = true;
    }
    // 入队成功后打印（此前在入队前打印，队列满时会误报“已发送”）。
    if (is_listen) {
        const char* state = message.find("\"state\":\"detect\"") != std::string_view::npos  ? "detect"
                            : message.find("\"state\":\"start\"") != std::string_view::npos ? "start"
                            : message.find("\"state\":\"stop\"") != std::string_view::npos  ? "stop"
                                                                                            : "?";
        const char* mode = message.find("\"mode\":\"auto\"") != std::string_view::npos       ? "auto"
                           : message.find("\"mode\":\"manual\"") != std::string_view::npos   ? "manual"
                           : message.find("\"mode\":\"realtime\"") != std::string_view::npos ? "realtime"
                                                                                             : "-";
        ESP_LOGI(detail::kTag, "LINX_SEND listen state=%s mode=%s", state, mode);
    } else if (is_abort) {
        ESP_LOGI(detail::kTag, "LINX_SEND abort");
    }
    return Status::Ok();
}

Status EspWebSocketTransport::Impl::SendAudio(voice::AudioFrame frame) {
    std::lock_guard<std::recursive_mutex> lifecycle_lock(lifecycle_mutex_);
    if (closing_.load() || client_ == nullptr || state_ != TransportState::kConnected || frame.payload.empty() ||
        frame.payload.size() > static_cast<size_t>(INT_MAX)) {
        return Status::Error(ErrorCode::kUnavailable, "ESP Linx Transport 尚未连接");
    }
    if (!media_tx_open_) {
        // Capture can race the state transition that enqueues listen.stop.
        // The frame is valid locally but must never cross that protocol fence.
        return Status::Ok();
    }
    // 统一 TX 队列：音频帧移入队后立即返回，网络写由 TxTask 执行，
    // 避免 esp_websocket_client_send_bin 同步阻塞 I2S 采集链导致大量丢帧。
    if (tx_queue_ == nullptr) {
        return Status::Error(ErrorCode::kUnavailable, "ESP Linx TX 队列未就绪");
    }
    auto* item = TryAcquireTxItem();
    if (item == nullptr) return Status::Error(ErrorCode::kUnavailable, "ESP Linx TX item pool 已满");
    item->kind = detail::LinxTxItem::Kind::kAudio;
    item->generation = frame.generation;
    item->sequence = frame.sequence;
    item->payload = std::move(frame.payload);
    // PCM is an ordered stream. Waiting briefly for the writer preserves the
    // sequence instead of evicting an older frame and making STT observe a
    // discontinuity. The capture producer is already decoupled by the audio
    // handoff queue, so this bounded wait cannot block the I2S read task.
    if (xQueueSend(tx_queue_, &item, pdMS_TO_TICKS(50)) != pdTRUE) {
        tx_audio_dropped_.fetch_add(1, std::memory_order_relaxed);
        ESP_LOGW(detail::kTag, "LINX_TX_AUDIO_DROP sequence=%llu queue_depth=%u",
                 static_cast<unsigned long long>(frame.sequence),
                 static_cast<unsigned>(uxQueueMessagesWaiting(tx_queue_)));
        ReleaseTxItem(item);
        return Status::Error(ErrorCode::kUnavailable, "ESP Linx TX 音频队列等待超时");
    }
    tx_audio_enqueued_.fetch_add(1, std::memory_order_relaxed);
    return Status::Ok();
}

Status EspWebSocketTransport::Impl::Close() {
    std::lock_guard<std::recursive_mutex> close_lock(close_mutex_);
    std::unique_lock<std::recursive_mutex> lifecycle_lock(lifecycle_mutex_);
    closing_.store(true);
    running_.store(false);
    state_ = TransportState::kDisconnected;
    media_tx_open_ = false;
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
    if (tx_task_ != nullptr) {
        // `esp_websocket_client_send_*` may still be executing after the
        // receive worker has drained. Wait for TxLoop to leave that call before
        // destroying the client handle it uses.
        const uint32_t wait_ms = options_.tx_timeout_ms + 1000U;
        if (xSemaphoreTake(tx_stopped_, pdMS_TO_TICKS(wait_ms)) != pdTRUE) {
            state_ = TransportState::kFailed;
            return status.ok() ? Status::Error(ErrorCode::kUnavailable, "等待 ESP Linx TX 任务退出超时") : status;
        }
        tx_task_ = nullptr;
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
        std::lock_guard<std::mutex> sink_lock(sink_mutex_);
        sink_ = {};
    }
    headers_.assign(headers_.size(), '\0');
    headers_.clear();
    return status;
}

void EspWebSocketTransport::Impl::SetGeneration(uint64_t generation) {
    std::lock_guard<std::recursive_mutex> lifecycle_lock(lifecycle_mutex_);
    // RX worker does not hold lifecycle_mutex_. Publish the invalidating
    // generation before waiting for an in-flight TX write, so an envelope
    // captured by the previous turn is rejected while this transition waits.
    generation_.store(generation, std::memory_order_release);
    media_tx_open_ = false;
    // An item may already have been dequeued by TxLoop. Advance generation
    // only after its check-and-write critical section has completed; otherwise
    // that old item could cross an interrupt or reconnect boundary.
    tx_generation_gate_.SetGeneration(generation);
    std::lock_guard<std::mutex> lock(assembler_mutex_);
    assembler_.Reset();
    // A new VoiceSession epoch invalidates every queued audio item. Drop the
    // backlog immediately so the next turn cannot wait behind old PCM.
    if (tx_queue_ != nullptr) {
        detail::LinxTxItem* stale = nullptr;
        while (xQueueReceive(tx_queue_, &stale, 0) == pdTRUE) {
            ReleaseTxItem(stale);
            stale = nullptr;
        }
    }
    if (tx_control_queue_ != nullptr) {
        detail::LinxTxItem* stale = nullptr;
        while (xQueueReceive(tx_control_queue_, &stale, 0) == pdTRUE) {
            ReleaseTxItem(stale);
            stale = nullptr;
        }
    }
}

detail::LinxTxItem* EspWebSocketTransport::Impl::TryAcquireTxItem() {
    std::unique_lock<std::mutex> lock(tx_item_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) return nullptr;
    for (std::size_t index = 0; index < tx_items_.size(); ++index) {
        if (tx_item_in_use_[index]) continue;
        tx_item_in_use_[index] = true;
        auto* item = &tx_items_[index];
        item->kind = detail::LinxTxItem::Kind::kText;
        item->generation = 0;
        item->sequence = 0;
        item->payload = voice::AudioPayload{};
        return item;
    }
    return nullptr;
}

void EspWebSocketTransport::Impl::ReleaseTxItem(detail::LinxTxItem* item) {
    if (item == nullptr) return;
    std::lock_guard<std::mutex> lock(tx_item_mutex_);
    const auto index = static_cast<std::size_t>(item - tx_items_.data());
    if (index >= tx_items_.size() || !tx_item_in_use_[index]) return;
    item->payload = voice::AudioPayload{};
    tx_item_in_use_[index] = false;
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
    tx_stopped_ = xSemaphoreCreateBinary();
    if (event_queue_ == nullptr || state_events_ == nullptr || worker_stopped_ == nullptr || tx_stopped_ == nullptr) {
        ESP_LOGW(detail::kTag, "LINX_WS_QUEUE_ALLOC_FAILED bytes=%u internal_free=%u",
                 static_cast<unsigned>(event_queue_bytes),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
        CleanupWorker();
        return false;
    }
    running_.store(true);
    // Process received frames ahead of the WebSocket client task so the
    // bounded queue drains during bursty STT/TTS traffic.
#if CONFIG_SPIRAM && (configSUPPORT_STATIC_ALLOCATION == 1)
    if (xTaskCreateWithCaps(&WorkerEntry, "linx_ws_events", options_.worker_task_stack_size, this, 6, &worker_,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
#else
    const uint32_t worker_stack_words = options_.worker_task_stack_size / sizeof(StackType_t);
    if (worker_stack_words == 0 ||
        xTaskCreate(&WorkerEntry, "linx_ws_events", worker_stack_words, this, 6, &worker_) != pdPASS) {
#endif
        CleanupWorker();
        return false;
    }
    // 统一 TX 队列：文本/音频/barrier 由唯一 TxTask 顺序发送。
    // 64 x 20 ms 约 1.28 s；音频满载时由 SendAudio 有界等待，不淘汰旧 PCM。
    // A long Chinese utterance can briefly outpace the TLS writer by more than
    // the previous 640 ms media FIFO. Keep this finite at 1.28 s; the item pool
    // is sized to cover this queue plus the control lane and in-flight write.
    constexpr int kTxQueueDepth = 64;
#if CONFIG_SPIRAM && (configSUPPORT_STATIC_ALLOCATION == 1)
    tx_queue_ = xQueueCreateWithCaps(kTxQueueDepth, sizeof(detail::LinxTxItem*), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    tx_queue_uses_caps_ = tx_queue_ != nullptr;
    if (tx_queue_ == nullptr) {
        ESP_LOGW(detail::kTag, "LINX_TX_QUEUE_PSRAM_ALLOC_FAILED");
    }
#endif
    if (tx_queue_ == nullptr) {
        tx_queue_ = xQueueCreate(kTxQueueDepth, sizeof(detail::LinxTxItem*));
    }
    // 文本/控制队列深度 16；音频队列满载不能阻塞这些语义消息。
    constexpr int kTxControlQueueDepth = 16;
#if CONFIG_SPIRAM && (configSUPPORT_STATIC_ALLOCATION == 1)
    tx_control_queue_ =
        xQueueCreateWithCaps(kTxControlQueueDepth, sizeof(detail::LinxTxItem*), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
    if (tx_control_queue_ == nullptr) {
        tx_control_queue_ = xQueueCreate(kTxControlQueueDepth, sizeof(detail::LinxTxItem*));
    }
    if (tx_control_queue_ == nullptr) {
        CleanupWorker();
        return false;
    }
    // TX 任务栈放 PSRAM（一次性分配、跨连接复用），TCB 在内部 RAM：
    // 16384B 动态任务栈常占内部 RAM，交互期断线重连时内部 RAM 最大连续块
    // 常 <16KB 导致重连任务创建失败（曾卡死聆听）；栈挪 PSRAM 腾出 16KB 头寸。
    constexpr uint32_t kTxStackWords = 16384 / sizeof(StackType_t);
    if (tx_stack_ == nullptr || tx_tcb_ == nullptr) {
        tx_stack_ = static_cast<StackType_t*>(
            heap_caps_malloc(sizeof(StackType_t) * kTxStackWords, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        tx_tcb_ =
            static_cast<StaticTask_t*>(heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        if (tx_stack_ == nullptr || tx_tcb_ == nullptr) {
            heap_caps_free(tx_stack_);
            heap_caps_free(tx_tcb_);
            tx_stack_ = nullptr;
            tx_tcb_ = nullptr;
            CleanupWorker();
            return false;
        }
    }
    tx_task_ = xTaskCreateStatic(&TxEntry, "linx_ws_tx", kTxStackWords, this, 5, tx_stack_, tx_tcb_);
    if (tx_queue_ == nullptr || tx_task_ == nullptr) {
        running_.store(false);
        detail::EventEnvelope shutdown;
        shutdown.kind = detail::EventKind::kShutdown;
        (void)xQueueSend(event_queue_, &shutdown, 0);
        if (worker_ != nullptr && xSemaphoreTake(worker_stopped_, pdMS_TO_TICKS(1000)) == pdTRUE) {
            worker_ = nullptr;
        }
        CleanupWorker();
        return false;
    }
    return true;
}

void EspWebSocketTransport::Impl::CleanupWorker() {
    if (worker_ != nullptr) {
        return;
    }
    if (tx_task_ != nullptr) {
        vTaskDelete(tx_task_);
        tx_task_ = nullptr;
    }
    if (tx_queue_ != nullptr) {
        // 丢弃仍排队的 TX 项，释放各自 payload。
        detail::LinxTxItem* pending = nullptr;
        while (xQueueReceive(tx_queue_, &pending, 0) == pdTRUE) {
            ReleaseTxItem(pending);
        }
#if CONFIG_SPIRAM && (configSUPPORT_STATIC_ALLOCATION == 1)
        if (tx_queue_uses_caps_) {
            vQueueDeleteWithCaps(tx_queue_);
        } else {
            vQueueDelete(tx_queue_);
        }
#else
        vQueueDelete(tx_queue_);
#endif
        tx_queue_ = nullptr;
        tx_queue_uses_caps_ = false;
    }
    if (tx_control_queue_ != nullptr) {
        detail::LinxTxItem* pending_ctl = nullptr;
        while (xQueueReceive(tx_control_queue_, &pending_ctl, 0) == pdTRUE) {
            ReleaseTxItem(pending_ctl);
        }
#if CONFIG_SPIRAM && (configSUPPORT_STATIC_ALLOCATION == 1)
        vQueueDeleteWithCaps(tx_control_queue_);
#else
        vQueueDelete(tx_control_queue_);
#endif
        tx_control_queue_ = nullptr;
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
    if (tx_stopped_ != nullptr) {
        vSemaphoreDelete(tx_stopped_);
        tx_stopped_ = nullptr;
    }
}

}  // namespace voicelife::linx_esp
