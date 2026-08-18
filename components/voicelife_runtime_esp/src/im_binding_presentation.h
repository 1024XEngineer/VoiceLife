#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "voicelife/im/im_binding_use_case.h"

namespace voicelife::runtime {

/// BoardRequest 为绑定系统播报预留的 UTF-8 字节数（含结尾空字符）。
constexpr std::size_t kBindingSystemSpeechCapacity = 96;
/// 绑定成功/失败等终态在 OLED 上的固定可见时长；不得依赖 TTS 完成事件清理。
constexpr uint32_t kBindingTerminalDisplayDurationMs = 3000;

/** 绑定状态映射出的纯用户呈现语义，不包含任何显示或语音硬件句柄。 */
struct BindingPresentation {
    /** true 时绑定码界面应在普通语音回合结束后恢复。 */
    bool keep_visible = false;
    /** true 时仅请求一次系统播报。 */
    bool announce = false;
    /** 大于零时终态页面到期后应独立退场，不依赖语音链路。 */
    uint32_t display_duration_ms = 0;
    /** true 时终态页面退场后开始采集，进入后续聆听。 */
    bool resume_listening = false;
    std::string status_text;
    std::string content_text;
    std::string speech_text;
};

/**
 * 将脱敏绑定结果转换为固定 OLED/TTS 文案。
 * 中间轮询状态刻意不产生输出，避免高频刷新和重复播报。
 */
BindingPresentation PresentBindingResult(const im::BindingResult& result);

/** @brief 仅当前 Runtime/会话代次的结果才允许进入设备呈现。 */
bool IsCurrentBindingResult(const im::BindingResult& result, uint64_t current_generation);

/** @brief 活跃语音回合返回绑定码后，播报结束应直接回待机，不进入 follow-up 聆听。 */
bool ShouldEndVoiceTurnAfterBindingResult(const im::BindingResult& result, bool active_voice_turn);

}  // namespace voicelife::runtime
