#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace voicelife::im {

/// 一个完整的 SSE 事件帧。
struct SseFrame {
    /// 事件 id（id: 字段），作为 Last-Event-ID 游标来源。
    std::string id;
    /// 事件类型（event: 字段），动作流中固定为 "reminder.action"。
    std::string event;
    /// 事件载荷（data: 字段），多行 data 以换行连接。
    std::string data;
};

/// 增量 SSE 帧解码器：喂入字节流，产出以空行分隔的完整事件帧。
///
/// 帧可跨多次 Feed 分片到达（ESP-IDF 流式读取的天然形态）；CRLF 与 LF 行尾
/// 均被归一化；心跳注释帧（以 ':' 开头）被忽略，不产出事件。
class SseDecoder {
   public:
    /**
     * @brief 向解码器喂入原始字节。
     * @param bytes 本次读到的字节。
     * @param frames 本次喂入后产出的完整事件帧；未完整帧保留到后续喂入。
     */
    void Feed(std::string_view bytes, std::vector<SseFrame>& frames);
    /** @brief 清空未完成帧的残留字节，连接复位时调用。 */
    void Reset();

   private:
    /// 解析一个以空行终止的帧块；仅有注释时返回 false 不产出事件。
    static bool ParseBlock(const std::string& block, SseFrame& frame);

    std::string buffer_;
};

}  // namespace voicelife::im
