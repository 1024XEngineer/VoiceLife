#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "voicelife/contracts/status.h"

namespace voicelife::linx_esp {

/** 表示本项目支持的 WebSocket 数据帧操作码。 */
enum class WebSocketOpcode : uint8_t {
    kContinuation = 0x0,
    kText = 0x1,
    kBinary = 0x2,
};

/** 表示一次 ESP-IDF WebSocket 数据回调中的有限分片。 */
struct WebSocketFragment {
    uint64_t generation = 0;
    WebSocketOpcode opcode = WebSocketOpcode::kContinuation;
    const uint8_t* data = nullptr;
    size_t data_len = 0;
    size_t payload_len = 0;
    size_t payload_offset = 0;
    bool fin = false;
};

/** 表示重组完成的一条 WebSocket 消息。 */
struct WebSocketMessage {
    uint64_t generation = 0;
    WebSocketOpcode opcode = WebSocketOpcode::kContinuation;
    std::vector<uint8_t> payload;
};

/** 表示一次分片推送是否完成以及完成后的消息。 */
struct WebSocketAssemblyResult {
    bool complete = false;
    WebSocketMessage message;
};

/** 在有界内存内重组 WebSocket 分片并校验代次。 */
class WebSocketFragmentAssembler final {
   public:
    /**
     * @brief 创建带消息大小上限的分片重组器。
     * @param max_message_bytes 允许的最大消息字节数。
     */
    explicit WebSocketFragmentAssembler(size_t max_message_bytes);

    /**
     * @brief 推入一个 WebSocket 分片。
     * @param fragment 待处理的分片。
     * @return 重组状态或错误。
     */
    Result<WebSocketAssemblyResult> Push(const WebSocketFragment& fragment);
    /** @brief 丢弃当前未完成的分片消息。 */
    void Reset();

    /** @brief 判断当前是否正在重组消息。 @return 正在重组时返回 true。 */
    [[nodiscard]] bool assembling() const { return assembling_; }

   private:
    Result<WebSocketAssemblyResult> Reject(ErrorCode code, const char* message);
    Result<WebSocketAssemblyResult> Complete();

    const size_t max_message_bytes_;
    bool assembling_ = false;
    uint64_t generation_ = 0;
    WebSocketOpcode opcode_ = WebSocketOpcode::kContinuation;
    size_t expected_payload_len_ = 0;
    std::vector<uint8_t> payload_;
};

}  // namespace voicelife::linx_esp
