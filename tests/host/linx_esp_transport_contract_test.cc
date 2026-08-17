#include <cstdint>
#include <string>
#include <string_view>

#include "support/test_support.h"
#include "voicelife/linx_esp/esp_websocket_transport.h"
#include "voicelife/linx_esp/websocket_fragment_assembler.h"

using voicelife::ErrorCode;
using voicelife::linx_esp::IsWebSocketDataOpcode;
using voicelife::linx_esp::WebSocketFragment;
using voicelife::linx_esp::WebSocketFragmentAssembler;
using voicelife::linx_esp::WebSocketOpcode;
using voicelife::test::Check;

namespace {

WebSocketFragment Chunk(uint64_t generation, WebSocketOpcode opcode, std::string_view payload, size_t payload_len,
                        size_t payload_offset, bool fin) {
    return {.generation = generation,
            .opcode = opcode,
            .data = reinterpret_cast<const uint8_t*>(payload.data()),
            .data_len = payload.size(),
            .payload_len = payload_len,
            .payload_offset = payload_offset,
            .fin = fin};
}

}  // namespace

int main() {
    Check(voicelife::linx_esp::EspWebSocketTransportOptions{}.max_message_bytes == 64 * 1024,
          "Linx WebSocket 默认消息上限必须为 64 KiB");

    WebSocketFragmentAssembler assembler(8);

    Check(IsWebSocketDataOpcode(WebSocketOpcode::kText) && IsWebSocketDataOpcode(WebSocketOpcode::kBinary) &&
              IsWebSocketDataOpcode(WebSocketOpcode::kContinuation),
          "text、binary 和 continuation 必须进入业务消息重组");
    Check(!IsWebSocketDataOpcode(static_cast<WebSocketOpcode>(0x8)) &&
              !IsWebSocketDataOpcode(static_cast<WebSocketOpcode>(0x9)) &&
              !IsWebSocketDataOpcode(static_cast<WebSocketOpcode>(0xA)),
          "close、ping 和 pong 控制帧不得进入业务消息重组");

    auto single = assembler.Push(Chunk(1, WebSocketOpcode::kText, "hello", 5, 0, true));
    Check(single.ok() && single.value->complete, "单帧 text 应立即完成");
    Check(single.value->message.generation == 1 && single.value->message.opcode == WebSocketOpcode::kText &&
              std::string(single.value->message.payload.begin(), single.value->message.payload.end()) == "hello",
          "单帧 text 内容和 generation 必须保留");

    auto first = assembler.Push(Chunk(2, WebSocketOpcode::kText, "hel", 5, 0, false));
    Check(first.ok() && !first.value->complete, "分片首帧不应提前完成");
    auto last = assembler.Push(Chunk(2, WebSocketOpcode::kContinuation, "lo", 5, 3, true));
    Check(last.ok() && last.value->complete &&
              std::string(last.value->message.payload.begin(), last.value->message.payload.end()) == "hello",
          "continuation 应拼接成完整 text");

    auto binary_first = assembler.Push(Chunk(3, WebSocketOpcode::kBinary, "ab", 4, 0, false));
    Check(binary_first.ok() && !binary_first.value->complete, "binary 首帧应进入组装状态");
    auto binary_last = assembler.Push(Chunk(3, WebSocketOpcode::kContinuation, "cd", 4, 2, true));
    Check(binary_last.ok() && binary_last.value->complete &&
              binary_last.value->message.opcode == WebSocketOpcode::kBinary,
          "binary continuation 应保留 binary opcode");

    auto orphan = assembler.Push(Chunk(4, WebSocketOpcode::kContinuation, "x", 1, 0, true));
    Check(orphan.status.code == ErrorCode::kInvalidArgument, "没有首帧的 continuation 必须拒绝");
    auto unsupported_opcode = assembler.Push({.generation = 4,
                                              .opcode = static_cast<WebSocketOpcode>(0x8),
                                              .data = reinterpret_cast<const uint8_t*>("x"),
                                              .data_len = 1,
                                              .payload_len = 1,
                                              .payload_offset = 0,
                                              .fin = true});
    Check(unsupported_opcode.status.code == ErrorCode::kInvalidArgument, "控制帧 opcode 不能进入业务消息 assembler");

    Check(assembler.Push(Chunk(5, WebSocketOpcode::kText, "a", 2, 0, false)).ok(), "非法序列测试前应先建立分片");
    auto interleaved = assembler.Push(Chunk(5, WebSocketOpcode::kBinary, "b", 1, 0, true));
    Check(interleaved.status.code == ErrorCode::kConflict, "分片中交错新 opcode 必须拒绝");
    Check(assembler.Push(Chunk(5, WebSocketOpcode::kText, "ok", 2, 0, true)).ok(),
          "拒绝交错帧后 assembler 必须可重新开始");

    Check(assembler.Push(Chunk(6, WebSocketOpcode::kText, "a", 3, 0, false)).ok(), "offset 测试前应先建立分片");
    auto bad_offset = assembler.Push(Chunk(6, WebSocketOpcode::kContinuation, "b", 3, 2, true));
    Check(bad_offset.status.code == ErrorCode::kConflict, "不连续 payload_offset 必须拒绝");

    Check(assembler.Push(Chunk(7, WebSocketOpcode::kText, "a", 2, 0, false)).ok(), "generation 测试前应先建立分片");
    auto stale = assembler.Push(Chunk(8, WebSocketOpcode::kContinuation, "b", 2, 1, true));
    Check(stale.status.code == ErrorCode::kConflict, "跨 generation continuation 必须丢弃");
    auto fresh = assembler.Push(Chunk(8, WebSocketOpcode::kText, "new", 3, 0, true));
    Check(fresh.ok() && fresh.value->complete, "丢弃旧 generation 后新消息必须可接收");

    auto too_large = assembler.Push(Chunk(9, WebSocketOpcode::kText, "123456789", 9, 0, true));
    Check(too_large.status.code == ErrorCode::kInvalidArgument, "超过消息上限必须拒绝");
    auto null_data = assembler.Push({.generation = 10,
                                     .opcode = WebSocketOpcode::kText,
                                     .data = nullptr,
                                     .data_len = 1,
                                     .payload_len = 1,
                                     .payload_offset = 0,
                                     .fin = true});
    Check(null_data.status.code == ErrorCode::kInvalidArgument, "非空数据不能使用空指针");

    // ESP-IDF 对超过 WS_BUFFER_SIZE 的单帧会分块投递多个 DATA 事件：
    // opcode/fin 保持帧头原值，payload_offset 单调递增、payload_len 不变。
    auto chunked_first = assembler.Push(Chunk(11, WebSocketOpcode::kBinary, "ab", 8, 0, true));
    Check(chunked_first.ok() && !chunked_first.value->complete, "超长二进制帧的首个分块必须进入组装状态且不提前完成");
    auto chunked_mid = assembler.Push(Chunk(11, WebSocketOpcode::kBinary, "cd", 8, 2, true));
    Check(chunked_mid.ok() && !chunked_mid.value->complete, "同帧后续分块应继续拼接");
    auto chunked_last = assembler.Push(Chunk(11, WebSocketOpcode::kBinary, "efgh", 8, 4, true));
    Check(chunked_last.ok() && chunked_last.value->complete &&
              std::string(chunked_last.value->message.payload.begin(), chunked_last.value->message.payload.end()) ==
                  "abcdefgh" &&
              chunked_last.value->message.opcode == WebSocketOpcode::kBinary,
          "超长二进制帧最后一个分块必须按声明长度完成并保留 opcode");

    auto chunked_bad_offset = assembler.Push(Chunk(12, WebSocketOpcode::kBinary, "a", 4, 0, true));
    Check(chunked_bad_offset.ok(), "分块 offset 校验前应先建立组装状态");
    auto chunked_wrong_offset = assembler.Push(Chunk(12, WebSocketOpcode::kBinary, "b", 4, 3, true));
    Check(chunked_wrong_offset.status.code == ErrorCode::kConflict, "同帧分块 offset 必须连续");
    auto chunked_len_first = assembler.Push(Chunk(12, WebSocketOpcode::kBinary, "ab", 4, 0, true));
    Check(chunked_len_first.ok(), "同帧分块 payload_len 校验前应先建立组装状态");
    auto chunked_wrong_len = assembler.Push(Chunk(12, WebSocketOpcode::kBinary, "b", 5, 2, true));
    Check(chunked_wrong_len.status.code == ErrorCode::kConflict, "同帧分块 payload_len 必须不变");

    assembler.Reset();
    Check(!assembler.assembling(), "Reset 必须清空未完成分片");
    return 0;
}
