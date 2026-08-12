#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "voicelife/linx_esp/esp_websocket_transport.h"
#include "voicelife/linx_esp/websocket_fragment_assembler.h"

namespace voicelife::linx_esp {
namespace detail {

constexpr char kTag[] = "voicelife_linx_esp";
constexpr size_t kMaxEventChunkBytes = 4096;
constexpr EventBits_t kConnectedBit = BIT0;
constexpr EventBits_t kFailedBit = BIT1;

enum class EventKind : uint8_t { kConnected, kData, kDisconnected, kError, kShutdown };

/** 统一 TX 队列项：文本/音频/barrier，由唯一 LinxTxTask 顺序发送。 */
struct LinxTxItem {
    enum class Kind : uint8_t { kText, kAudio, kBarrier };
    Kind kind = Kind::kText;
    std::vector<uint8_t> payload;
    uint64_t generation = 0;
};

struct EventEnvelope {
    EventKind kind = EventKind::kError;
    uint64_t generation = 0;
    uint8_t opcode = 0;
    bool fin = false;
    size_t data_len = 0;
    size_t payload_len = 0;
    size_t payload_offset = 0;
    int tls_last_error = 0;
    int tls_stack_error = 0;
    int tls_cert_flags = 0;
    int handshake_status = 0;
    int socket_errno = 0;
    std::array<uint8_t, kMaxEventChunkBytes> data{};
};

}  // namespace detail

class EspWebSocketTransport::Impl final {
   public:
    Impl(SecretResolverPort& secrets, EspWebSocketTransportOptions options)
        : secrets_(secrets), options_(std::move(options)), assembler_(options_.max_message_bytes) {}

    ~Impl() { Close(); }

    Status Connect(const linx::LinxConnectionConfig& config, linx::LinxTransportSink sink);
    Status SendText(std::string_view message);
    Status SendAudio(const voice::AudioFrame& frame);
    Status Close();
    void SetGeneration(uint64_t generation);
    TransportState state() const { return state_.load(); }

   private:
    static bool ValidHeaderValue(std::string_view value);
    static Result<std::string> BuildHeaders(const linx::LinxConnectionConfig& config, std::string_view token);
    bool PrepareWorker();
    void CleanupWorker();
    static void OnEvent(void* handler_args, esp_event_base_t, int32_t event_id, void* event_data);
    void Enqueue(int32_t event_id, const esp_websocket_event_data_t* event_data);
    static void WorkerEntry(void* argument);
    void WorkerLoop();
    static void TxEntry(void* argument);
    void TxLoop();
    void HandleQueueOverflow();
    void HandleEnvelope(const detail::EventEnvelope& envelope);
    void HandleData(const detail::EventEnvelope& envelope);
    linx::LinxTransportSink SinkSnapshot();

    SecretResolverPort& secrets_;
    EspWebSocketTransportOptions options_;
    esp_websocket_client_handle_t client_ = nullptr;
    QueueHandle_t event_queue_ = nullptr;
    bool event_queue_uses_caps_ = false;
    // 唯一 TX 队列：文本/音频/barrier 统一由 TxTask 顺序发送，
    // TLS 只在 TxTask 运行（栈 16KB），避免调用任务同步写阻塞。
    QueueHandle_t tx_queue_ = nullptr;
    // 高优先级控制队列：listen.stop/abort 等控制帧，音频占满时仍可入队。
    QueueHandle_t tx_control_queue_ = nullptr;
    bool tx_queue_uses_caps_ = false;
    TaskHandle_t tx_task_ = nullptr;
    EventGroupHandle_t state_events_ = nullptr;
    SemaphoreHandle_t worker_stopped_ = nullptr;
    TaskHandle_t worker_ = nullptr;
    std::atomic<bool> running_{false};
    std::atomic<bool> closing_{false};
    std::atomic<bool> accepting_events_{false};
    std::atomic<bool> queue_overflowed_{false};
    std::atomic<bool> connect_waiting_{false};
    std::atomic<uint64_t> generation_{0};
    std::atomic<TransportState> state_{TransportState::kDisconnected};
    std::recursive_mutex lifecycle_mutex_;
    std::recursive_mutex close_mutex_;
    std::mutex assembler_mutex_;
    std::mutex callback_mutex_;
    std::mutex status_mutex_;
    WebSocketFragmentAssembler assembler_;
    linx::LinxTransportSink sink_;
    std::string headers_;
    Status error_status_ = Status::Ok();
};

}  // namespace voicelife::linx_esp
