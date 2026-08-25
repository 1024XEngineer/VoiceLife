#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include "support/test_support.h"
#include "voicelife/linx_esp/esp_websocket_transport.h"
#include "voicelife/linx_esp/linx_tx_generation_gate.h"
#include "voicelife/linx_esp/linx_tx_policy.h"
#include "voicelife/linx_esp/websocket_fragment_assembler.h"

using voicelife::ErrorCode;
using voicelife::linx_esp::IsWebSocketDataOpcode;
using voicelife::linx_esp::LinxTextTxLane;
using voicelife::linx_esp::LinxTxGenerationGate;
using voicelife::linx_esp::SelectLinxTextTxLane;
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
    const voicelife::linx_esp::EspWebSocketTransportOptions defaults{};
    Check(defaults.websocket_task_stack_size == 6144,
          "WebSocket task stack must fit the ESP32-S3 internal heap after SQLite startup");
    Check(defaults.worker_task_stack_size == 32 * 1024,
          "Linx worker task stack must provide 32 KiB for SQLite/FATFS operations");
    Check(defaults.network_timeout_ms == 10000, "Linx 网络超时必须保留 10 秒，避免慢速下行分片被错误重连");
    Check(defaults.tx_timeout_ms == 1000, "Linx 默认同步写超时必须限制在 1 秒，避免 generation 切换拖慢本地打断");
    Check(SelectLinxTextTxLane("{\"type\":\"listen\",\"state\":\"stop\"}") == LinxTextTxLane::kMediaOrdered,
          "listen.stop 必须排在已经入队的 PCM 之后，不能由控制队列越过尾音");
    Check(SelectLinxTextTxLane("{\"type\":\"listen\",\"state\":\"start\"}") == LinxTextTxLane::kMediaOrdered &&
              SelectLinxTextTxLane("{\"type\":\"listen\",\"state\":\"detect\"}") == LinxTextTxLane::kControl &&
              SelectLinxTextTxLane("{\"type\":\"abort\"}") == LinxTextTxLane::kControl,
          "start 必须与 PCM 有序，detect 和 abort 仍保留控制通道的低延迟抢占能力");

    LinxTxGenerationGate generation_gate;
    generation_gate.SetGeneration(1);
    std::mutex generation_mutex;
    std::condition_variable generation_cv;
    bool old_write_started = false;
    bool release_old_write = false;
    std::atomic<bool> generation_switched = false;
    std::thread old_write([&]() {
        Check(generation_gate.SendIfCurrent(1,
                                            [&]() {
                                                std::unique_lock<std::mutex> lock(generation_mutex);
                                                old_write_started = true;
                                                generation_cv.notify_all();
                                                generation_cv.wait(lock, [&]() { return release_old_write; });
                                            }),
              "当前 generation 的写入必须允许开始");
    });
    {
        std::unique_lock<std::mutex> lock(generation_mutex);
        generation_cv.wait(lock, [&]() { return old_write_started; });
    }
    std::thread switch_generation([&]() {
        generation_gate.SetGeneration(2);
        generation_switched.store(true);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    Check(!generation_switched.load(), "generation 切换必须等待已经开始的同步写完成");
    {
        std::lock_guard<std::mutex> lock(generation_mutex);
        release_old_write = true;
    }
    generation_cv.notify_all();
    old_write.join();
    switch_generation.join();
    Check(generation_switched.load() && !generation_gate.SendIfCurrent(1, []() {}),
          "generation 切换完成后已经出队的旧帧不得再开始写入");

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
