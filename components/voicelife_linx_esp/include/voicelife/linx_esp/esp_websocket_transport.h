#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "voicelife/linx/linx_types.h"

namespace voicelife::linx_esp {

/** 定义从安全存储解析密钥引用的端口。 */
class SecretResolverPort {
   public:
    /** @brief 允许通过接口类型释放密钥解析端口。 */
    virtual ~SecretResolverPort() = default;
    /**
     * @brief 解析一个密钥引用。
     * @param reference 非敏感的密钥引用。
     * @return 解析出的密钥或错误。
     */
    virtual Result<std::string> Resolve(std::string_view reference) = 0;
};

/** 表示 ESP WebSocket 传输的生命周期状态。 */
enum class TransportState {
    kDisconnected,
    kConnecting,
    kConnected,
    kReconnecting,
    kFailed,
};

/** 配置 ESP WebSocket 传输的容量、超时和安全策略。 */
struct EspWebSocketTransportOptions {
    // 上限只在分片重组时按实际消息长度占用；64 KiB 可容纳较长的下行控制/文本帧。
    size_t max_message_bytes = 64 * 1024;
    // A single envelope owns up to 4 KiB of frame data. 256 entries absorb a
    // bounded Opus burst while the PCM output port applies playback backpressure.
    size_t event_queue_capacity = 256;
    size_t event_chunk_bytes = 4096;
    uint32_t connect_timeout_ms = 10000;
    // ESP-IDF applies this to receive operations as well as network I/O.
    // Keep fragmented downstream audio and control frames on the normal
    // network budget; TX uses tx_timeout_ms below.
    uint32_t network_timeout_ms = 10000;
    // A generation switch serializes with an in-flight write. Keep that wait
    // bounded so a stalled socket cannot defer a local barge-in for the old
    // connection budget.
    uint32_t tx_timeout_ms = 1000;
    uint32_t reconnect_timeout_ms = 1000;
    // ESP-IDF passes this value to xTaskCreatePinnedToCore in StackType_t
    // words, not bytes. 4096 words (16 KiB on ESP32-S3) leaves room for the
    // TLS task and I2S DMA after SQLite/FATFS startup.
    uint32_t websocket_task_stack_size = 6144;
    // MCP 日程工具（schedule.create/query）在此 worker 任务上执行 SQLite/FATFS 操作，
    // SQLite 的 sqlite3_step 需要较大栈，12KB 会导致栈溢出崩溃。
    // xTaskCreateWithCaps interprets this value in bytes. 32 KiB is enough
    // for sqlite3_step; the stack is allocated in PSRAM so it does not
    // consume the audio driver's internal heap.
    uint32_t worker_task_stack_size = 32 * 1024;
    bool enable_close_reconnect = true;
    bool allow_insecure_ws = false;
};

/** 使用 ESP-IDF WebSocket 客户端实现 Linx 传输端口。 */
class EspWebSocketTransport final : public linx::LinxTransportPort {
   public:
    /**
     * @brief 创建 ESP WebSocket 传输。
     * @param secrets 提供密钥解析的端口。
     * @param options 传输容量、超时和安全选项。
     */
    EspWebSocketTransport(SecretResolverPort& secrets, EspWebSocketTransportOptions options = {});
    /** @brief 释放 WebSocket 传输资源。 */
    ~EspWebSocketTransport() override;

    /**
     * @brief 禁止复制 WebSocket 传输。
     * @param other 复制源对象。
     */
    EspWebSocketTransport(const EspWebSocketTransport& other) = delete;
    /**
     * @brief 禁止赋值 WebSocket 传输。
     * @param other 赋值源对象。
     * @return 当前对象引用。
     */
    EspWebSocketTransport& operator=(const EspWebSocketTransport& other) = delete;

    /**
     * @brief 建立 Linx WebSocket 连接。
     * @param config Linx 连接配置。
     * @param sink 接收传输事件的回调。
     * @return 连接结果。
     */
    Status Connect(const linx::LinxConnectionConfig& config, linx::LinxTransportSink sink) override;
    /**
     * @brief 发送文本控制消息。
     * @param message 待发送文本。
     * @return 发送结果。
     */
    Status SendText(std::string_view message) override;
    /**
     * @brief 发送一帧音频数据。
     * @param frame 待发送音频帧。
     * @return 发送结果。
     */
    Status SendAudio(voice::AudioFrame frame) override;
    /**
     * @brief 关闭 WebSocket 连接。
     * @return 关闭结果。
     */
    Status Close() override;
    /**
     * @brief 设置当前会话代次。
     * @param generation 当前会话代次。
     */
    void SetGeneration(uint64_t generation) override;

    /**
     * @brief 返回当前 WebSocket 传输状态。
     * @return 传输状态。
     */
    [[nodiscard]] TransportState state() const;

   private:
    /** 隐藏 ESP-IDF 细节的传输实现。 */
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace voicelife::linx_esp
