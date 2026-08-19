#pragma once

#include <string_view>

namespace voicelife::linx_esp {

/** 表示 Linx 文本帧在发送侧应使用的有界队列。 */
enum class LinxTextTxLane { kControl, kMediaOrdered };

/**
 * @brief 为 Linx 文本控制帧选择发送队列。
 *
 * listen.stop 是当前上行语音的结束边界，必须排在已采集 PCM 之后；
 * abort 则需要立即抢占旧音频。因此只有前者进入媒体 FIFO。
 *
 * @param message 已编码的 Linx 文本控制帧。
 * @return listen.stop 返回 kMediaOrdered，其他文本返回 kControl。
 */
[[nodiscard]] inline LinxTextTxLane SelectLinxTextTxLane(std::string_view message) {
    const bool is_listen = message.find("\"type\":\"listen\"") != std::string_view::npos;
    const bool is_listen_stop = is_listen && message.find("\"state\":\"stop\"") != std::string_view::npos;
    return is_listen_stop ? LinxTextTxLane::kMediaOrdered : LinxTextTxLane::kControl;
}

}  // namespace voicelife::linx_esp
