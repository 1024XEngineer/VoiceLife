#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "voicelife/contracts/status.h"

namespace voicelife::linx_esp {

enum class WebSocketOpcode : uint8_t {
    kContinuation = 0x0,
    kText = 0x1,
    kBinary = 0x2,
};

// A copied slice of one esp_websocket_client DATA event. `payload_len` and
// `payload_offset` describe the complete WebSocket message, while data points
// only to this callback's bounded slice.
struct WebSocketFragment {
    uint64_t generation = 0;
    WebSocketOpcode opcode = WebSocketOpcode::kContinuation;
    const uint8_t* data = nullptr;
    size_t data_len = 0;
    size_t payload_len = 0;
    size_t payload_offset = 0;
    bool fin = false;
};

struct WebSocketMessage {
    uint64_t generation = 0;
    WebSocketOpcode opcode = WebSocketOpcode::kContinuation;
    std::vector<uint8_t> payload;
};

struct WebSocketAssemblyResult {
    bool complete = false;
    WebSocketMessage message;
};

class WebSocketFragmentAssembler final {
   public:
    explicit WebSocketFragmentAssembler(size_t max_message_bytes);

    Result<WebSocketAssemblyResult> Push(const WebSocketFragment& fragment);
    void Reset();

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
