#pragma once

#include <string_view>

namespace voicelife::linx_esp {

/** 表示 Linx 文本帧在发送侧应使用的有界队列。 */
enum class LinxTextTxLane { kControl, kMediaOrdered };

/**
 * @brief 为 Linx 文本控制帧选择发送队列。
 *
 * listen.start/stop 是当前上行语音的起止边界，必须与 PCM 共享同一 FIFO；
 * abort 则需要立即抢占旧音频，因此继续进入控制 FIFO。
 *
 * @param message 已编码的 Linx 文本控制帧。
 * @return listen.start/stop 返回 kMediaOrdered，其他文本返回 kControl。
 */
[[nodiscard]] inline LinxTextTxLane SelectLinxTextTxLane(std::string_view message) {
    const bool is_listen = message.find("\"type\":\"listen\"") != std::string_view::npos;
    const bool is_listen_boundary =
        is_listen && (message.find("\"state\":\"start\"") != std::string_view::npos ||
                      message.find("\"state\":\"stop\"") != std::string_view::npos);
    return is_listen_boundary ? LinxTextTxLane::kMediaOrdered : LinxTextTxLane::kControl;
}

}  // namespace voicelife::linx_esp
