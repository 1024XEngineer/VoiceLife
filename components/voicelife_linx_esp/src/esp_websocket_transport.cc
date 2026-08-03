#include "voicelife/linx_esp/esp_websocket_transport.h"

#include "voicelife/linx_esp/websocket_fragment_assembler.h"

#include <array>
#include <atomic>
#include <climits>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>

#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace voicelife::linx_esp {
namespace {

constexpr char kTag[] = "voicelife_linx_esp";
constexpr size_t kMaxEventChunkBytes = 4096;
constexpr EventBits_t kConnectedBit = BIT0;
constexpr EventBits_t kFailedBit = BIT1;

enum class EventKind : uint8_t { kConnected, kData, kDisconnected, kError, kShutdown };

struct EventEnvelope {
    EventKind kind = EventKind::kError;
    uint8_t opcode = 0;
    bool fin = false;
    size_t data_len = 0;
    size_t payload_len = 0;
    size_t payload_offset = 0;
    std::array<uint8_t, kMaxEventChunkBytes> data{};
};

Status EspError(const char* operation, esp_err_t error) {
    return Status::Error(ErrorCode::kUnavailable,
                         std::string(operation) + " 失败，esp_err_t=" + std::to_string(error));
}

}  // namespace

class EspWebSocketTransport::Impl final {
   public:
    Impl(SecretResolverPort& secrets, EspWebSocketTransportOptions options)
        : secrets_(secrets), options_(std::move(options)), assembler_(options_.max_message_bytes) {}

    ~Impl() { Close(); }

    Status Connect(const linx::LinxConnectionConfig& config, linx::LinxTransportSink sink) {
        if (!config.valid() || config.websocket_url.rfind("wss://", 0) != 0 ||
            options_.event_queue_capacity == 0 || options_.event_chunk_bytes == 0 ||
            options_.event_chunk_bytes > kMaxEventChunkBytes || options_.max_message_bytes == 0) {
            return Status::Error(ErrorCode::kInvalidArgument, "ESP Linx WSS 配置无效");
        }
        if (state_ == TransportState::kConnecting || state_ == TransportState::kConnected ||
            state_ == TransportState::kReconnecting) {
            return Status::Error(ErrorCode::kConflict, "ESP Linx Transport 已连接");
        }
        if (state_ == TransportState::kFailed) {
            Close();
        }

        auto token = secrets_.Resolve(config.token_ref);
        if (!token.ok() || !token.value.has_value() || token.value->empty()) {
            return token.ok() ? Status::Error(ErrorCode::kInvalidArgument, "Linx token 为空")
                              : token.status;
        }
        headers_ = BuildHeaders(config, *token.value);
        state_ = TransportState::kConnecting;
        closing_.store(false);
        sink_ = std::move(sink);
        error_status_ = Status::Ok();
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
        websocket_config.buffer_size = static_cast<int>(options_.event_chunk_bytes);
        websocket_config.crt_bundle_attach = esp_crt_bundle_attach;
        websocket_config.skip_cert_common_name_check = false;
        websocket_config.user_context = this;

        client_ = esp_websocket_client_init(&websocket_config);
        if (client_ == nullptr) {
            CleanupWorker();
            state_ = TransportState::kFailed;
            return Status::Error(ErrorCode::kInternal, "ESP WebSocket client 初始化失败");
        }
        esp_err_t status = esp_websocket_register_events(client_, WEBSOCKET_EVENT_ANY, &OnEvent, this);
        if (status != ESP_OK) {
            Close();
            state_ = TransportState::kFailed;
            return EspError("注册 WebSocket 事件", status);
        }
        status = esp_websocket_client_start(client_);
        if (status != ESP_OK) {
            Close();
            state_ = TransportState::kFailed;
            return EspError("启动 WebSocket client", status);
        }

        const EventBits_t bits = xEventGroupWaitBits(
            state_events_, kConnectedBit | kFailedBit, pdTRUE, pdFALSE,
            pdMS_TO_TICKS(options_.connect_timeout_ms));
        if ((bits & kConnectedBit) != 0) {
            return Status::Ok();
        }
        const Status failure = error_status_;
        Close();
        state_ = TransportState::kFailed;
        if (!failure.ok()) {
            return failure;
        }
        return Status::Error(ErrorCode::kUnavailable, "ESP Linx WSS 连接超时");
    }

    Status SendText(std::string_view message) {
        if (client_ == nullptr || state_ != TransportState::kConnected ||
            message.size() > static_cast<size_t>(INT_MAX)) {
            return Status::Error(ErrorCode::kUnavailable, "ESP Linx Transport 尚未连接或消息过大");
        }
        const int sent = esp_websocket_client_send_text(client_, message.data(),
                                                        static_cast<int>(message.size()),
                                                        pdMS_TO_TICKS(options_.network_timeout_ms));
        return sent < 0 ? Status::Error(ErrorCode::kUnavailable, "发送 Linx 文本消息失败")
                        : Status::Ok();
    }

    Status SendAudio(const voice::AudioFrame& frame) {
        if (client_ == nullptr || state_ != TransportState::kConnected || frame.payload.empty() ||
            frame.payload.size() > static_cast<size_t>(INT_MAX)) {
            return Status::Error(ErrorCode::kUnavailable, "ESP Linx Transport 尚未连接");
        }
        const int sent = esp_websocket_client_send_bin(
            client_, reinterpret_cast<const char*>(frame.payload.data()),
            static_cast<int>(frame.payload.size()), pdMS_TO_TICKS(options_.network_timeout_ms));
        return sent < 0 ? Status::Error(ErrorCode::kUnavailable, "发送 Linx 音频失败") : Status::Ok();
    }

    Status Close() {
        closing_.store(true);
        Status status = Status::Ok();
        if (client_ != nullptr) {
            const esp_err_t stop_status = esp_websocket_client_stop(client_);
            if (stop_status != ESP_OK && stop_status != ESP_ERR_INVALID_STATE) {
                status = EspError("停止 WebSocket client", stop_status);
            }
        }
        running_.store(false);
        if (event_queue_ != nullptr) {
            EventEnvelope shutdown;
            shutdown.kind = EventKind::kShutdown;
            xQueueSend(event_queue_, &shutdown, 0);
        }
        if (worker_ != nullptr && xTaskGetCurrentTaskHandle() != worker_) {
            xSemaphoreTake(worker_stopped_, pdMS_TO_TICKS(1000));
            worker_ = nullptr;
        }
        if (client_ != nullptr) {
            const esp_err_t destroy_status = esp_websocket_client_destroy(client_);
            client_ = nullptr;
            if (status.ok() && destroy_status != ESP_OK) {
                status = EspError("销毁 WebSocket client", destroy_status);
            }
        }
        CleanupWorker();
        assembler_.Reset();
        sink_ = {};
        headers_.assign(headers_.size(), '\0');
        headers_.clear();
        state_ = TransportState::kDisconnected;
        return status;
    }

    void SetGeneration(uint64_t generation) {
        generation_.store(generation);
        std::lock_guard<std::mutex> lock(assembler_mutex_);
        assembler_.Reset();
    }

    TransportState state() const { return state_.load(); }

   private:
    static std::string BuildHeaders(const linx::LinxConnectionConfig& config,
                                    std::string_view token) {
        std::string authorization(token);
        if (authorization.rfind("Bearer ", 0) != 0) {
            authorization.insert(0, "Bearer ");
        }
        return "Authorization: " + authorization + "\r\nProtocol-Version: 1\r\nDevice-Id: " +
               config.device_id + "\r\nClient-Id: " + config.client_id + "\r\n";
    }

    bool PrepareWorker() {
        event_queue_ = xQueueCreate(options_.event_queue_capacity, sizeof(EventEnvelope));
        state_events_ = xEventGroupCreate();
        worker_stopped_ = xSemaphoreCreateBinary();
        if (event_queue_ == nullptr || state_events_ == nullptr || worker_stopped_ == nullptr) {
            CleanupWorker();
            return false;
        }
        running_.store(true);
        if (xTaskCreate(&WorkerEntry, "linx_ws_events", 6144, this, 5, &worker_) != pdPASS) {
            CleanupWorker();
            return false;
        }
        return true;
    }

    void CleanupWorker() {
        if (worker_ != nullptr && xTaskGetCurrentTaskHandle() != worker_) {
            vTaskDelete(worker_);
            worker_ = nullptr;
        }
        if (event_queue_ != nullptr) {
            vQueueDelete(event_queue_);
            event_queue_ = nullptr;
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

    static void OnEvent(void* handler_args, esp_event_base_t, int32_t event_id, void* event_data) {
        static_cast<Impl*>(handler_args)->Enqueue(event_id,
                                                  static_cast<esp_websocket_event_data_t*>(event_data));
    }

    void Enqueue(int32_t event_id, const esp_websocket_event_data_t* event_data) {
        if (event_queue_ == nullptr) {
            return;
        }
        EventEnvelope envelope;
        if (event_id == WEBSOCKET_EVENT_CONNECTED) {
            envelope.kind = EventKind::kConnected;
        } else if (event_id == WEBSOCKET_EVENT_DISCONNECTED) {
            envelope.kind = EventKind::kDisconnected;
        } else if (event_id == WEBSOCKET_EVENT_DATA && event_data != nullptr) {
            envelope.kind = EventKind::kData;
            envelope.opcode = event_data->op_code;
            envelope.fin = event_data->fin;
            envelope.data_len = event_data->data_len;
            envelope.payload_len = event_data->payload_len;
            envelope.payload_offset = event_data->payload_offset;
            if (event_data->data_len > options_.event_chunk_bytes || event_data->data_ptr == nullptr) {
                envelope.kind = EventKind::kError;
                envelope.data_len = 0;
            } else if (event_data->data_len > 0) {
                std::memcpy(envelope.data.data(), event_data->data_ptr, event_data->data_len);
            }
        } else if (event_id == WEBSOCKET_EVENT_ERROR) {
            envelope.kind = EventKind::kError;
        } else {
            return;
        }
        if (xQueueSend(event_queue_, &envelope, 0) != pdTRUE) {
            ESP_LOGW(kTag, "Linx WebSocket 事件队列已满，丢弃事件");
            queue_overflowed_.store(true);
            state_ = TransportState::kFailed;
            xEventGroupSetBits(state_events_, kFailedBit);
        }
    }

    static void WorkerEntry(void* argument) {
        static_cast<Impl*>(argument)->WorkerLoop();
        vTaskDelete(nullptr);
    }

    void WorkerLoop() {
        EventEnvelope envelope;
        while (running_.load() && xQueueReceive(event_queue_, &envelope, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (queue_overflowed_.exchange(false)) {
                HandleQueueOverflow();
            }
            if (envelope.kind == EventKind::kShutdown) {
                break;
            }
            HandleEnvelope(envelope);
        }
        if (worker_stopped_ != nullptr) {
            xSemaphoreGive(worker_stopped_);
        }
    }

    void HandleQueueOverflow() {
        const Status status = Status::Error(ErrorCode::kUnavailable,
                                            "ESP Linx WebSocket 事件队列溢出");
        error_status_ = status;
        if (sink_.on_error) {
            sink_.on_error(status);
        }
    }

    void HandleEnvelope(const EventEnvelope& envelope) {
        switch (envelope.kind) {
            case EventKind::kConnected:
                state_ = TransportState::kConnected;
                xEventGroupSetBits(state_events_, kConnectedBit);
                return;
            case EventKind::kDisconnected:
                {
                    std::lock_guard<std::mutex> lock(assembler_mutex_);
                    assembler_.Reset();
                }
                state_ = closing_.load() ? TransportState::kDisconnected
                                         : TransportState::kReconnecting;
                return;
            case EventKind::kError:
                error_status_ = Status::Error(ErrorCode::kUnavailable,
                                              "ESP Linx WebSocket 收到错误事件");
                state_ = TransportState::kFailed;
                xEventGroupSetBits(state_events_, kFailedBit);
                if (sink_.on_error) {
                    sink_.on_error(error_status_);
                }
                return;
            case EventKind::kData:
                HandleData(envelope);
                return;
            case EventKind::kShutdown:
                return;
        }
    }

    void HandleData(const EventEnvelope& envelope) {
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
            error_status_ = failure;
            if (sink_.on_error) {
                sink_.on_error(failure);
            }
            return;
        }
        if (!assembled.complete) {
            return;
        }
        if (assembled.message.opcode == WebSocketOpcode::kText) {
            if (sink_.on_text) {
                const auto& payload = assembled.message.payload;
                sink_.on_text(std::string_view(reinterpret_cast<const char*>(payload.data()), payload.size()));
            }
        } else if (assembled.message.opcode == WebSocketOpcode::kBinary) {
            if (sink_.on_binary) {
                sink_.on_binary(assembled.message.payload);
            }
        }
    }

    SecretResolverPort& secrets_;
    EspWebSocketTransportOptions options_;
    esp_websocket_client_handle_t client_ = nullptr;
    QueueHandle_t event_queue_ = nullptr;
    EventGroupHandle_t state_events_ = nullptr;
    SemaphoreHandle_t worker_stopped_ = nullptr;
    TaskHandle_t worker_ = nullptr;
    std::atomic<bool> running_{false};
    std::atomic<bool> closing_{false};
    std::atomic<bool> queue_overflowed_{false};
    std::atomic<uint64_t> generation_{0};
    std::atomic<TransportState> state_{TransportState::kDisconnected};
    std::mutex assembler_mutex_;
    WebSocketFragmentAssembler assembler_;
    linx::LinxTransportSink sink_;
    std::string headers_;
    Status error_status_ = Status::Ok();
};

EspWebSocketTransport::EspWebSocketTransport(SecretResolverPort& secrets,
                                             EspWebSocketTransportOptions options)
    : impl_(std::make_unique<Impl>(secrets, std::move(options))) {}

EspWebSocketTransport::~EspWebSocketTransport() = default;

Status EspWebSocketTransport::Connect(const linx::LinxConnectionConfig& config,
                                      linx::LinxTransportSink sink) {
    return impl_->Connect(config, std::move(sink));
}

Status EspWebSocketTransport::SendText(std::string_view message) {
    return impl_->SendText(message);
}

Status EspWebSocketTransport::SendAudio(const voice::AudioFrame& frame) {
    return impl_->SendAudio(frame);
}

Status EspWebSocketTransport::Close() { return impl_->Close(); }

void EspWebSocketTransport::SetGeneration(uint64_t generation) { impl_->SetGeneration(generation); }

TransportState EspWebSocketTransport::state() const { return impl_->state(); }

}  // namespace voicelife::linx_esp
