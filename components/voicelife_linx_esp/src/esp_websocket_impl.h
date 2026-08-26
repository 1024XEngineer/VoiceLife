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
#include "voicelife/linx_esp/linx_tx_generation_gate.h"
#include "voicelife/linx_esp/websocket_fragment_assembler.h"

namespace voicelife::linx_esp {
namespace detail {

constexpr char kTag[] = "voicelife_linx_esp";
constexpr size_t kMaxEventChunkBytes = 4096;
// 64 media + 16 control + one in-flight writer + one stop item. Audio items
// hold pooled PCM leases, so this remains bounded while absorbing the measured
// sub-second TLS stalls without silently evicting an earlier speech frame.
constexpr size_t kTxItemPoolCapacity = 82;
constexpr EventBits_t kConnectedBit = BIT0;
constexpr EventBits_t kFailedBit = BIT1;

enum class EventKind : uint8_t { kConnected, kData, kDisconnected, kError, kShutdown };

/** 统一 TX 队列项：文本/音频/barrier，由唯一 LinxTxTask 顺序发送。 */
struct LinxTxItem {
    enum class Kind : uint8_t { kText, kAudio, kBarrier };
    Kind kind = Kind::kText;
    voice::AudioPayload payload;
    uint64_t generation = 0;
    uint64_t sequence = 0;
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
    int close_status_code = 0;
    std::array<uint8_t, kMaxEventChunkBytes> data{};
};

}  // namespace detail

class EspWebSocketTransport::Impl final {
   public:
    Impl(SecretResolverPort& secrets, EspWebSocketTransportOptions options)
        : secrets_(secrets), options_(std::move(options)), assembler_(options_.max_message_bytes) {}

    ~Impl();

    Status Connect(const linx::LinxConnectionConfig& config, linx::LinxTransportSink sink);
    Status SendText(std::string_view message);
    Status SendAudio(voice::AudioFrame frame);
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
    detail::LinxTxItem* TryAcquireTxItem();
    void ReleaseTxItem(detail::LinxTxItem* item);
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
    std::array<detail::LinxTxItem, detail::kTxItemPoolCapacity> tx_items_{};
    std::array<bool, detail::kTxItemPoolCapacity> tx_item_in_use_{};
    std::mutex tx_item_mutex_;
    bool tx_queue_uses_caps_ = false;
    // listen.stop closes the media lane for the current generation. Late
    // capture frames are discarded instead of being sent after the stop frame.
    bool media_tx_open_ = false;
    uint64_t tx_audio_sent_ = 0;
    std::atomic<uint64_t> tx_audio_enqueued_{0};
    std::atomic<uint64_t> tx_audio_dropped_{0};
    TaskHandle_t tx_task_ = nullptr;
    // linx_ws_tx 任务栈常驻 PSRAM、TCB 在内部 RAM（一次性分配、跨连接复用，随
    // Transport 生命周期）：交互（采集+音频流）期间内部 RAM 最大连续块常 <16KB，
    // 16384B 动态任务栈在 WS 断线重连时创建会失败（重连窗口内部 RAM 最紧），把栈
    // 挪到 PSRAM 腾出 16KB 内部头寸，保证 esp_websocket_client 内建重连任务
    // （~4KB 栈）能分配。TCB 必须内部 RAM（xPortCheckValidTCBMem 断言）。
    StackType_t* tx_stack_ = nullptr;
    StaticTask_t* tx_tcb_ = nullptr;
    EventGroupHandle_t state_events_ = nullptr;
    SemaphoreHandle_t worker_stopped_ = nullptr;
    // Close must not destroy `client_` while TxLoop is inside a synchronous
    // WebSocket write. The task signals this semaphore after it has observed
    // `running_ == false` and released every client access.
    SemaphoreHandle_t tx_stopped_ = nullptr;
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
    std::mutex sink_mutex_;
    std::mutex status_mutex_;
    LinxTxGenerationGate tx_generation_gate_;
    WebSocketFragmentAssembler assembler_;
    linx::LinxTransportSink sink_;
    std::string headers_;
    Status error_status_ = Status::Ok();
};

}  // namespace voicelife::linx_esp
