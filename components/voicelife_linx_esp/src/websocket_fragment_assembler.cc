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
        if (fragment.payload_offset != 0 || (fragment.fin && fragment.data_len != fragment.payload_len) ||
            (!fragment.fin && (fragment.payload_len == 0 || fragment.data_len == 0))) {
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
        if (fragment.opcode != WebSocketOpcode::kContinuation || fragment.payload_len != expected_payload_len_ ||
            fragment.payload_offset != payload_.size()) {
            return Reject(ErrorCode::kConflict, "WebSocket continuation 序列无效");
        }
    }

    if (fragment.data_len > 0) {
        payload_.insert(payload_.end(), fragment.data, fragment.data + fragment.data_len);
    }
    if (fragment.fin) {
        if (payload_.size() != expected_payload_len_) {
            return Reject(ErrorCode::kInvalidArgument, "WebSocket 完整帧长度不匹配");
        }
        return Complete();
    }
    if (payload_.size() >= expected_payload_len_) {
        return Reject(ErrorCode::kInvalidArgument, "未结束的 WebSocket 分片已达到声明长度");
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
