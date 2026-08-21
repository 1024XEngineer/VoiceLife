#pragma once

#include <cstdint>

#include "voicelife/voice/display_snapshot.h"

namespace voicelife::display_sparkbot {

/**
 * @brief 判断一份快照是否允许进入待机屏保。
 * @param snapshot 当前显示快照。
 * @return 快照仅包含纯待机内容时返回 true。
 */
[[nodiscard]] inline bool IsIdleScreenSaverEligible(const voicelife::voice::DisplaySnapshot& snapshot) {
    return snapshot.phase == voicelife::voice::VoiceInteractionState::kStandby &&
           snapshot.role == voicelife::voice::VoiceContentRole::kNone && snapshot.content_text.empty();
}

/**
 * @brief 判断待机空闲时间是否已达到屏保阈值。
 * @param snapshot 当前显示快照。
 * @param idle_elapsed_ms 当前待机已持续的毫秒数。
 * @param timeout_ms 屏保进入阈值，零表示不启用。
 * @return 快照允许屏保且空闲时间达到阈值时返回 true。
 */
[[nodiscard]] inline bool ShouldEnterIdleScreenSaver(const voicelife::voice::DisplaySnapshot& snapshot,
                                                     uint32_t idle_elapsed_ms, uint32_t timeout_ms) {
    return timeout_ms > 0 && IsIdleScreenSaverEligible(snapshot) && idle_elapsed_ms >= timeout_ms;
}

/**
 * @brief 判断高优先级显示内容是否要求退出屏保。
 * @param snapshot 当前显示快照。
 * @return 快照不再是纯待机内容时返回 true。
 */
[[nodiscard]] inline bool ShouldExitIdleScreenSaver(const voicelife::voice::DisplaySnapshot& snapshot) {
    return snapshot.phase != voicelife::voice::VoiceInteractionState::kStandby ||
           snapshot.role != voicelife::voice::VoiceContentRole::kNone || !snapshot.content_text.empty();
}

}  // namespace voicelife::display_sparkbot
