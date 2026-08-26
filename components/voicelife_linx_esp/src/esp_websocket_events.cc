#include <cstring>
#include <string>
#include <string_view>
#include <utility>

#include "esp_log.h"
#include "esp_tls_errors.h"
#include "esp_websocket_client.h"
#include "esp_websocket_impl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

namespace voicelife::linx_esp {

void EspWebSocketTransport::Impl::OnEvent(void* handler_args, esp_event_base_t, int32_t event_id, void* event_data) {
    static_cast<Impl*>(handler_args)->Enqueue(event_id, static_cast<esp_websocket_event_data_t*>(event_data));
}

void EspWebSocketTransport::Impl::Enqueue(int32_t event_id, const esp_websocket_event_data_t* event_data) {
    std::lock_guard<std::mutex> callback_lock(callback_mutex_);
    if (!accepting_events_.load() || event_queue_ == nullptr) {
        return;
    }
    detail::EventEnvelope envelope;
    envelope.generation = generation_.load();
    if (event_id == WEBSOCKET_EVENT_CONNECTED) {
        envelope.kind = detail::EventKind::kConnected;
    } else if (event_id == WEBSOCKET_EVENT_DISCONNECTED) {
        envelope.kind = detail::EventKind::kDisconnected;
    } else if (event_id == WEBSOCKET_EVENT_DATA && event_data != nullptr) {
        // ESP-IDF dispatches ping, pong and close control frames through the
        // same DATA event. The managed client handles those frames itself;
        // only RFC 6455 data opcodes belong in the Linx message assembler.
        if (!IsWebSocketDataOpcode(static_cast<WebSocketOpcode>(event_data->op_code))) {
            return;
        }
        envelope.kind = detail::EventKind::kData;
        envelope.opcode = event_data->op_code;
        envelope.fin = event_data->fin;
        envelope.data_len = event_data->data_len;
        envelope.payload_len = event_data->payload_len;
        envelope.payload_offset = event_data->payload_offset;
        if (event_data->data_len > options_.event_chunk_bytes || event_data->data_ptr == nullptr) {
            envelope.kind = detail::EventKind::kError;
            envelope.data_len = 0;
        } else if (event_data->data_len > 0) {
            std::memcpy(envelope.data.data(), event_data->data_ptr, event_data->data_len);
        }
    } else if (event_id == WEBSOCKET_EVENT_ERROR) {
        // 服务端有序关闭是正常告别，不当故障：
        // - 收到 WebSocket CLOSE 帧（SERVER_CLOSE）
        // - TCP 有序 FIN（esp-tls 报 TCP_CLOSED_FIN）
        // 均映射为 kDisconnected（触发自动重连），其余才是真正故障（证书/握手/超时）。
        const auto error_type = event_data != nullptr ? event_data->error_handle.error_type : WEBSOCKET_ERROR_TYPE_NONE;
        const bool tcp_transport_error = error_type == WEBSOCKET_ERROR_TYPE_TCP_TRANSPORT;
        // ESP-IDF leaves error_handle diagnostic members unspecified for some
        // ERROR_TYPE_NONE callbacks (notably peer TCP RST/SSL read failure).
        // Never interpret those bytes as a TLS failure: doing so turns a
        // recoverable disconnect into provider_error and an error screen.
        const bool diagnostics_valid = event_data != nullptr && error_type != WEBSOCKET_ERROR_TYPE_NONE;
        const int handshake_status = diagnostics_valid ? event_data->error_handle.esp_ws_handshake_status_code : 0;
        const int tls_last_error = diagnostics_valid ? event_data->error_handle.esp_tls_last_esp_err : 0;
        const int tls_stack_error = diagnostics_valid ? event_data->error_handle.esp_tls_stack_err : 0;
        const int tls_cert_flags = diagnostics_valid ? event_data->error_handle.esp_tls_cert_verify_flags : 0;
        const int socket_errno = diagnostics_valid ? event_data->error_handle.esp_transport_sock_errno : 0;
        const bool handshake_failed = handshake_status != 0;
        const bool tls_failed = tls_last_error != 0 || tls_stack_error != 0 || tls_cert_flags != 0;
        const bool ordered_close = error_type == WEBSOCKET_ERROR_TYPE_SERVER_CLOSE ||
                                   (tcp_transport_error && event_data != nullptr &&
                                    event_data->error_handle.esp_tls_last_esp_err == ESP_ERR_ESP_TLS_TCP_CLOSED_FIN);
        // On ESP-IDF, a peer TCP RST can arrive as ERROR_TYPE_NONE with all
        // diagnostic fields zero. It is still a lost WebSocket connection and
        // must enter the reconnect path. Handshake/TLS failures retain the
        // error path so invalid credentials and certificates are not retried
        // as if the session had been cleanly disconnected.
        const bool retryable_transport_loss =
            ordered_close ||
            (event_data != nullptr && !handshake_failed && !tls_failed &&
             (tcp_transport_error || error_type == WEBSOCKET_ERROR_TYPE_NONE) && event_data->close_status_code == 0);
        if (event_data != nullptr) {
            ESP_LOGW(detail::kTag,
                     "LINX_WS_ERROR_EVENT event=ERROR classified=%s type=%u close=%d handshake=%d tls_valid=%d tls=%d "
                     "stack=%d cert_flags=%d errno=%d",
                     retryable_transport_loss ? "disconnect" : "error", static_cast<unsigned>(error_type),
                     event_data->close_status_code, handshake_status, diagnostics_valid ? 1 : 0, tls_last_error,
                     tls_stack_error, tls_cert_flags, socket_errno);
        } else {
            ESP_LOGW(detail::kTag, "LINX_WS_ERROR_EVENT event=ERROR classified=error type=%u close=0 event_data=null",
                     static_cast<unsigned>(error_type));
        }
        if (retryable_transport_loss) {
            envelope.kind = detail::EventKind::kDisconnected;
            envelope.opcode = static_cast<uint8_t>(error_type);
        } else {
            envelope.kind = detail::EventKind::kError;
            if (event_data != nullptr) {
                envelope.handshake_status = handshake_status;
                envelope.close_status_code = event_data->close_status_code;
                envelope.tls_last_error = tls_last_error;
                envelope.tls_stack_error = tls_stack_error;
                envelope.tls_cert_flags = tls_cert_flags;
                envelope.socket_errno = socket_errno;
                envelope.opcode = static_cast<uint8_t>(error_type);
            }
        }
    } else {
        return;
    }
    // If playback is applying backpressure, wait briefly for the worker to
    // drain a slot so the TCP receive path slows the server instead of
    // silently dropping a TTS frame. callback_mutex_ is intentionally not
    // used by SinkSnapshot; holding it while waiting would deadlock the
    // worker on a full queue.
    if (xQueueSend(event_queue_, &envelope, pdMS_TO_TICKS(500)) != pdTRUE) {
        ESP_LOGW(detail::kTag, "Linx WebSocket 事件队列已满，丢弃事件");
        queue_overflowed_.store(true);
        state_ = TransportState::kFailed;
        xEventGroupSetBits(state_events_, detail::kFailedBit);
    }
}

void EspWebSocketTransport::Impl::WorkerEntry(void* argument) {
    static_cast<Impl*>(argument)->WorkerLoop();
#if CONFIG_SPIRAM && (configSUPPORT_STATIC_ALLOCATION == 1)
    vTaskDeleteWithCaps(nullptr);
#else
    vTaskDelete(nullptr);
#endif
}

void EspWebSocketTransport::Impl::WorkerLoop() {
    detail::EventEnvelope envelope;
    while (running_.load()) {
        if (xQueueReceive(event_queue_, &envelope, pdMS_TO_TICKS(100)) != pdTRUE) {
            continue;
        }
        if (queue_overflowed_.exchange(false)) {
            HandleQueueOverflow();
        }
        if (envelope.kind == detail::EventKind::kShutdown) {
            break;
        }
        HandleEnvelope(envelope);
    }
    if (worker_stopped_ != nullptr) {
        xSemaphoreGive(worker_stopped_);
    }
}

void EspWebSocketTransport::Impl::HandleQueueOverflow() {
    const Status status = Status::Error(ErrorCode::kUnavailable, "ESP Linx WebSocket 事件队列溢出");
    {
        std::lock_guard<std::mutex> status_lock(status_mutex_);
        error_status_ = status;
    }
    const linx::LinxTransportSink sink = SinkSnapshot();
    if (sink.on_error) {
        sink.on_error(status);
    }
}

void EspWebSocketTransport::Impl::TxEntry(void* argument) {
    static_cast<Impl*>(argument)->TxLoop();
    vTaskDelete(nullptr);
}

void EspWebSocketTransport::Impl::TxLoop() {
    // 唯一 TX 任务：按队列顺序发送文本/音频，TLS 只在本任务运行。
    // 独立的短 TX 超时避免写阻塞拖垮采集；网络接收仍使用其正常预算。
    uint64_t last_audio_generation = 0;
    uint64_t last_audio_sequence = 0;
    bool have_audio_sequence = false;
    while (running_.load()) {
        detail::LinxTxItem* item = nullptr;
        // 控制命令优先；作为音频结束边界的 listen.stop 已进入媒体 FIFO，
        // 因而仍排在本轮已入队 PCM 之后。
        if (tx_control_queue_ != nullptr && xQueueReceive(tx_control_queue_, &item, 0) == pdTRUE) {
            // 从控制队列取到 item，直接发送。
        } else if (tx_queue_ != nullptr && xQueueReceive(tx_queue_, &item, pdMS_TO_TICKS(100)) != pdTRUE) {
            continue;
        }
        if (item == nullptr) {
            continue;
        }
        if (item->kind == detail::LinxTxItem::Kind::kBarrier) {
            // barrier：文本/音频序列的分界（如 listen.stop 排在本轮音频之后）。
            ReleaseTxItem(item);
            continue;
        }
        int sent = -1;
        const bool sent_current = tx_generation_gate_.SendIfCurrent(item->generation, [this, &item, &sent]() {
            sent = item->kind == detail::LinxTxItem::Kind::kText
                       ? esp_websocket_client_send_text(client_, reinterpret_cast<const char*>(item->payload.data()),
                                                        static_cast<int>(item->payload.size()),
                                                        pdMS_TO_TICKS(options_.tx_timeout_ms))
                       : esp_websocket_client_send_bin(client_, reinterpret_cast<const char*>(item->payload.data()),
                                                       static_cast<int>(item->payload.size()),
                                                       pdMS_TO_TICKS(options_.tx_timeout_ms));
        });
        const size_t want = item->payload.size();
        const auto kind = item->kind;
        const uint64_t generation = item->generation;
        const uint64_t sequence = item->sequence;
        ReleaseTxItem(item);
        item = nullptr;
        if (!sent_current) {
            continue;
        }
        if (sent < 0 || static_cast<size_t>(sent) != want) {
            // 发送失败时交给 ESP-IDF 客户端自己的自动重连状态机。TX 任务不能
            // 并发 stop/start，否则会与客户端重连任务竞争并丢失后续唤醒。
            ESP_LOGW(detail::kTag, "LINX_TX_SEND_FAIL sent=%d want=%u, await client auto-reconnect", sent,
                     static_cast<unsigned>(want));
            // 本次连接的媒体和控制命令都不能穿过重连边界。仅清理 PCM
            // 会让失效的 listen.start/abort 在新连接上被错误发送。
            detail::LinxTxItem* remaining = nullptr;
            while (tx_queue_ != nullptr && xQueueReceive(tx_queue_, &remaining, 0) == pdTRUE) {
                ReleaseTxItem(remaining);
                remaining = nullptr;
            }
            while (tx_control_queue_ != nullptr && xQueueReceive(tx_control_queue_, &remaining, 0) == pdTRUE) {
                ReleaseTxItem(remaining);
                remaining = nullptr;
            }
            continue;
        }
        if (kind == detail::LinxTxItem::Kind::kAudio) {
            // sequence restarts at zero for every listen.start media round. A
            // new round is a boundary, not a missing frame in the previous one.
            const bool new_media_round = sequence == 0 || generation != last_audio_generation;
            if (have_audio_sequence && !new_media_round && sequence != last_audio_sequence + 1) {
                ESP_LOGW(detail::kTag, "LINX_TX_AUDIO_GAP previous=%llu current=%llu generation=%llu",
                         static_cast<unsigned long long>(last_audio_sequence),
                         static_cast<unsigned long long>(sequence), static_cast<unsigned long long>(generation));
            }
            last_audio_generation = generation;
            last_audio_sequence = sequence;
            have_audio_sequence = true;
            ++tx_audio_sent_;
            if (tx_audio_sent_ <= 3 || tx_audio_sent_ % 20 == 0) {
                ESP_LOGI(detail::kTag, "LINX_TX_AUDIO_SENT count=%llu sequence=%llu bytes=%u",
                         static_cast<unsigned long long>(tx_audio_sent_), static_cast<unsigned long long>(sequence),
                         static_cast<unsigned>(want));
            }
        } else {
            ESP_LOGI(detail::kTag, "LINX_TX_TEXT_SENT bytes=%u", static_cast<unsigned>(want));
        }
    }
    if (tx_stopped_ != nullptr) {
        xSemaphoreGive(tx_stopped_);
    }
}

void EspWebSocketTransport::Impl::HandleEnvelope(const detail::EventEnvelope& envelope) {
    if (envelope.generation != generation_.load()) {
        return;
    }
    const linx::LinxTransportSink sink = SinkSnapshot();
    switch (envelope.kind) {
        case detail::EventKind::kConnected:
            state_ = TransportState::kConnected;
            if (sink.on_connected) {
                sink.on_connected();
            }
            xEventGroupSetBits(state_events_, detail::kConnectedBit);
            return;
        case detail::EventKind::kDisconnected: {
            std::lock_guard<std::mutex> lock(assembler_mutex_);
            assembler_.Reset();
        }
            state_ = closing_.load() ? TransportState::kDisconnected : TransportState::kReconnecting;
            if (sink.on_disconnected) {
                sink.on_disconnected();
            }
            return;
        case detail::EventKind::kError: {
            ESP_LOGW(detail::kTag,
                     "LINX_WS_ERROR event=worker type=%u close=%d tls=%d stack=%d cert_flags=%d handshake=%d errno=%d",
                     static_cast<unsigned>(envelope.opcode), envelope.close_status_code, envelope.tls_last_error,
                     envelope.tls_stack_error, envelope.tls_cert_flags, envelope.handshake_status,
                     envelope.socket_errno);
            std::lock_guard<std::mutex> status_lock(status_mutex_);
            error_status_ = Status::Error(ErrorCode::kUnavailable, "ESP Linx WebSocket 收到错误事件");
        }
            state_ = TransportState::kFailed;
            xEventGroupSetBits(state_events_, detail::kFailedBit);
            if (sink.on_error) {
                sink.on_error(Status::Error(ErrorCode::kUnavailable, "ESP Linx WebSocket 收到错误事件"));
            }
            return;
        case detail::EventKind::kData:
            HandleData(envelope);
            return;
        case detail::EventKind::kShutdown:
            return;
    }
}

void EspWebSocketTransport::Impl::HandleData(const detail::EventEnvelope& envelope) {
    if (envelope.generation != generation_.load()) {
        return;
    }
    const linx::LinxTransportSink sink = SinkSnapshot();
    if (envelope.opcode == static_cast<uint8_t>(WebSocketOpcode::kBinary) &&
        envelope.payload_len > voice::AudioFrame::kMaxPayloadBytes) {
        const Status failure = Status::Error(ErrorCode::kInvalidArgument, "Linx 二进制音频消息超过单帧内存上限");
        {
            std::lock_guard<std::mutex> status_lock(status_mutex_);
            error_status_ = failure;
        }
        if (sink.on_error) sink.on_error(failure);
        return;
    }
    WebSocketAssemblyResult assembled;
    Status failure = Status::Ok();
    {
        std::lock_guard<std::mutex> lock(assembler_mutex_);
        auto result = assembler_.Push({.generation = generation_.load(),
                                       .opcode = static_cast<WebSocketOpcode>(envelope.opcode),
                                       .data = envelope.data.data(),
                                       .data_len = envelope.data_len,
                                       .payload_len = envelope.payload_len,
                                       .payload_offset = envelope.payload_offset,
                                       .fin = envelope.fin});
        if (!result.ok() || !result.value.has_value()) {
            failure = result.status;
        } else {
            assembled = std::move(*result.value);
        }
    }
    if (!failure.ok()) {
        {
            std::lock_guard<std::mutex> status_lock(status_mutex_);
            error_status_ = failure;
        }
        if (sink.on_error) {
            sink.on_error(failure);
        }
        return;
    }
    if (!assembled.complete) {
        return;
    }
    if (assembled.message.opcode == WebSocketOpcode::kText) {
        if (sink.on_text) {
            const auto& payload = assembled.message.payload;
            sink.on_text(std::string_view(reinterpret_cast<const char*>(payload.data()), payload.size()));
        }
    } else if (assembled.message.opcode == WebSocketOpcode::kBinary) {
        if (sink.on_binary) {
            sink.on_binary(std::move(assembled.message.payload));
        }
    }
}

linx::LinxTransportSink EspWebSocketTransport::Impl::SinkSnapshot() {
    std::lock_guard<std::mutex> sink_lock(sink_mutex_);
    return sink_;
}

}  // namespace voicelife::linx_esp
