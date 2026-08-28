#pragma once

#include <cstdint>
#include <string>

#include "voicelife/voice/voice_interaction_state.h"

namespace voicelife::voice {

/** @brief 牛头表情键（显示模型层使用，与具体渲染器解耦）。 */
enum class VoiceMood {
    /** 开机资源/服务尚未就绪。 */
    kBooting,
    /** 等待配网或设备绑定。 */
    kProvisioning,
    /** 网络或 Voice Provider 正在建立连接。 */
    kConnecting,
    /** 可再次唤醒的稳定待机态。 */
    kIdle,
    /** 已打开麦克风，正在接收用户语音。 */
    kListening,
    kNeutral,
    kHappy,
    kSad,
    kThinking,
    kSurprised,
    kSpeaking,
    /** 当前回合被用户或服务端取消，随后应恢复待机。 */
    kCancelled,
    kAngry,
};

/** @brief 内容栏当前展示的文本角色。 */
enum class VoiceContentRole {
    kNone,
    kSystem,
    kUser,
    kAssistant,
};

/**
 * @brief 显示模型快照：一次会话阶段变化后派生出的完整可见状态。
 *
 * 由 Runtime 维护，仅在 revision 变化时提交给渲染器，避免全屏重绘。
 * 只保留产品语义（阶段、表情、文本角色和文本），禁止包含 LVGL 对象、
 * 图形框架句柄、SPI/I2C 参数、文件路径、URL 或像素缓冲区；具体渲染由
 * 显示 Adapter 在其专属上下文中完成。
 */
struct DisplaySnapshot {
    /** @brief 语义回合代次（会话切换后递增，用于丢弃旧回合迟到快照）。 */
    uint64_t generation = 0;
    VoiceInteractionState phase = VoiceInteractionState::kBooting;
    VoiceMood mood = VoiceMood::kNeutral;
    /** @brief Linx 常用 emotion key；未知值由 Renderer 回退到 mood。 */
    std::string emotion_key;
    /** 当前网络是否已获得可用连接；由 Runtime 写入，Renderer 仅据此显示图标。 */
    bool network_connected = false;
    /** 上行状态栏文本（如“聆听中...”“处理中...”）。 */
    std::string status_text;
    /** 下行内容栏文本（用户语音 / 助手回复 / 系统提示）。 */
    std::string content_text;
    VoiceContentRole role = VoiceContentRole::kNone;
    uint64_t revision = 0;
};

}  // namespace voicelife::voice
