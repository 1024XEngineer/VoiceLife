#include <cstring>
#include <string>
#include <string_view>
#include <utility>

#include "esp_log.h"
#include "esp_tls_errors.h"
#include "esp_websocket_client.h"
#include "esp_websocket_impl.h"
#include "freertos/FreeRTOS.h"
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
        const bool ordered_close =
            error_type == WEBSOCKET_ERROR_TYPE_SERVER_CLOSE ||
            (event_data != nullptr && event_data->error_handle.esp_tls_last_esp_err == ESP_ERR_ESP_TLS_TCP_CLOSED_FIN);
        if (ordered_close) {
            envelope.kind = detail::EventKind::kDisconnected;
            envelope.opcode = static_cast<uint8_t>(error_type);
        } else {
            envelope.kind = detail::EventKind::kError;
            if (event_data != nullptr) {
                envelope.tls_last_error = event_data->error_handle.esp_tls_last_esp_err;
                envelope.tls_stack_error = event_data->error_handle.esp_tls_stack_err;
                envelope.tls_cert_flags = event_data->error_handle.esp_tls_cert_verify_flags;
                envelope.handshake_status = event_data->error_handle.esp_ws_handshake_status_code;
                envelope.socket_errno = event_data->error_handle.esp_transport_sock_errno;
                envelope.opcode = static_cast<uint8_t>(error_type);
            }
        }
    } else {
        return;
    }
    if (xQueueSend(event_queue_, &envelope, 0) != pdTRUE) {
        ESP_LOGW(detail::kTag, "Linx WebSocket 事件队列已满，丢弃事件");
        queue_overflowed_.store(true);
        state_ = TransportState::kFailed;
        xEventGroupSetBits(state_events_, detail::kFailedBit);
    }
}

void EspWebSocketTransport::Impl::WorkerEntry(void* argument) {
    static_cast<Impl*>(argument)->WorkerLoop();
    vTaskDelete(nullptr);
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
            delete item;
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
        delete item;
        item = nullptr;
        if (!sent_current) {
            continue;
        }
        if (sent < 0 || static_cast<size_t>(sent) != want) {
            // 发送失败（写阻塞/短写/连接已断）：不能直接 esp_websocket_client_stop
            // ——stop 会停止客户端，ESP 内建自动重连（disable_auto_reconnect=false）
            // 随之失效，Session 永久卡在非 Ready（无法二次唤醒/说话）。
            // 正确做法：停止后立即重启 client，让内建自动重连继续负责重连
            // （单一重连执行者），随后断开事件会走 transport_disconnected 恢复。
            ESP_LOGW(detail::kTag, "LINX_TX_SEND_FAIL sent=%d want=%u, restart client for reconnect", sent,
                     static_cast<unsigned>(want));
            if (client_ != nullptr && !closing_.load()) {
                (void)esp_websocket_client_stop(client_);
                // 重启以恢复内建自动重连；start 会重新进入连接流程并自动重连。
                (void)esp_websocket_client_start(client_);
            }
            // 本次连接的媒体和控制命令都不能穿过重连边界。仅清理 PCM
            // 会让失效的 listen.start/abort 在新连接上被错误发送。
            detail::LinxTxItem* remaining = nullptr;
            while (tx_queue_ != nullptr && xQueueReceive(tx_queue_, &remaining, 0) == pdTRUE) {
                delete remaining;
                remaining = nullptr;
            }
            while (tx_control_queue_ != nullptr && xQueueReceive(tx_control_queue_, &remaining, 0) == pdTRUE) {
                delete remaining;
                remaining = nullptr;
            }
            continue;
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
            ESP_LOGW(detail::kTag, "LINX_WS_ERROR type=%u tls=%d stack=%d cert_flags=%d handshake=%d errno=%d",
                     static_cast<unsigned>(envelope.opcode), envelope.tls_last_error, envelope.tls_stack_error,
                     envelope.tls_cert_flags, envelope.handshake_status, envelope.socket_errno);
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
    std::lock_guard<std::mutex> callback_lock(callback_mutex_);
    return sink_;
}

}  // namespace voicelife::linx_esp
