#include <cstring>
#include <string>
#include <string_view>
#include <utility>

#include "esp_log.h"
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
        envelope.kind = detail::EventKind::kError;
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
            sink.on_binary(assembled.message.payload);
        }
    }
}

linx::LinxTransportSink EspWebSocketTransport::Impl::SinkSnapshot() {
    std::lock_guard<std::mutex> callback_lock(callback_mutex_);
    return sink_;
}

}  // namespace voicelife::linx_esp
