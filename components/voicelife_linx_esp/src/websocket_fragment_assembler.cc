#include "voicelife/linx_esp/websocket_fragment_assembler.h"

#include <limits>

namespace voicelife::linx_esp {

WebSocketFragmentAssembler::WebSocketFragmentAssembler(size_t max_message_bytes)
    : max_message_bytes_(max_message_bytes) {}

Result<WebSocketAssemblyResult> WebSocketFragmentAssembler::Reject(ErrorCode code, const char* message) {
    Reset();
    return Result<WebSocketAssemblyResult>::Failure(code, message);
}

Result<WebSocketAssemblyResult> WebSocketFragmentAssembler::Complete() {
    WebSocketAssemblyResult result;
    result.complete = true;
    result.message.generation = generation_;
    result.message.opcode = opcode_;
    result.message.payload = std::move(payload_);
    Reset();
    return Result<WebSocketAssemblyResult>::Success(std::move(result));
}

Result<WebSocketAssemblyResult> WebSocketFragmentAssembler::Push(const WebSocketFragment& fragment) {
    if (max_message_bytes_ == 0 || fragment.payload_len > max_message_bytes_ ||
        fragment.data_len > max_message_bytes_ || fragment.payload_offset > fragment.payload_len ||
        fragment.data_len > fragment.payload_len - fragment.payload_offset ||
        (fragment.data_len > 0 && fragment.data == nullptr)) {
        return Reject(ErrorCode::kInvalidArgument, "WebSocket 分片边界无效");
    }

    if (!assembling_) {
        if (fragment.opcode == WebSocketOpcode::kContinuation) {
            return Reject(ErrorCode::kInvalidArgument, "没有首帧的 continuation");
        }
        if (fragment.opcode != WebSocketOpcode::kText && fragment.opcode != WebSocketOpcode::kBinary) {
            return Reject(ErrorCode::kInvalidArgument, "WebSocket opcode 不支持");
        }
        if (fragment.payload_offset != 0 || fragment.payload_len == 0) {
            return Reject(ErrorCode::kInvalidArgument, "WebSocket 首帧边界无效");
        }
        assembling_ = true;
        generation_ = fragment.generation;
        opcode_ = fragment.opcode;
        expected_payload_len_ = fragment.payload_len;
        payload_.reserve(expected_payload_len_);
    } else {
        if (fragment.generation != generation_) {
            return Reject(ErrorCode::kConflict, "WebSocket 分片 generation 已过期");
        }
        // ESP-IDF 对超过 WS_BUFFER_SIZE 的单帧会分块投递多个 DATA 事件：
        // 每块 opcode/fin 保持帧头原值，payload_offset 单调递增、payload_len 不变。
        const bool same_frame_chunk = fragment.opcode == opcode_ && fragment.payload_len == expected_payload_len_;
        const bool protocol_continuation =
            fragment.opcode == WebSocketOpcode::kContinuation && fragment.payload_len == expected_payload_len_;
        if (!same_frame_chunk && !protocol_continuation) {
            return Reject(ErrorCode::kConflict, "WebSocket continuation 序列无效");
        }
        if (fragment.payload_offset != payload_.size()) {
            return Reject(ErrorCode::kConflict, "WebSocket continuation 序列无效");
        }
    }

    if (fragment.data_len > 0) {
        payload_.insert(payload_.end(), fragment.data, fragment.data + fragment.data_len);
    }
    if (payload_.size() == expected_payload_len_) {
        return Complete();
    }
    if (payload_.size() > expected_payload_len_) {
        return Reject(ErrorCode::kInvalidArgument, "WebSocket 完整帧长度不匹配");
    }
    return Result<WebSocketAssemblyResult>::Success({});
}

void WebSocketFragmentAssembler::Reset() {
    assembling_ = false;
    generation_ = 0;
    opcode_ = WebSocketOpcode::kContinuation;
    expected_payload_len_ = 0;
    payload_.clear();
}

}  // namespace voicelife::linx_esp
