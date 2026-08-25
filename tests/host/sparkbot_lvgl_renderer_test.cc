#include "voicelife/display_sparkbot/sparkbot_lvgl_renderer.h"

#include "support/test_support.h"
#include "voicelife/display_sparkbot/sparkbot_lvgl_display.h"
#include "voicelife/display_sparkbot/sparkbot_screen_saver.h"
#include "voicelife/voice/display_snapshot.h"

using voicelife::ErrorCode;
using voicelife::test::Check;
using voicelife::voice::DisplaySnapshot;
using voicelife::voice::VoiceMood;

int main() {
    using voicelife::display_sparkbot::DefaultSparkBotDisplayLayout;
    using voicelife::display_sparkbot::EmotionKeyForMood;
    using voicelife::display_sparkbot::IsCommonLinxEmojiKey;
    using voicelife::display_sparkbot::IsIdleScreenSaverEligible;
    using voicelife::display_sparkbot::IsValidLogicalSpiHost;
    using voicelife::display_sparkbot::ShouldEnterIdleScreenSaver;
    using voicelife::display_sparkbot::ShouldExitIdleScreenSaver;
    using voicelife::display_sparkbot::SparkBotLvglRenderer;
    using voicelife::display_sparkbot::ScrollDurationForOverflow;

    // 官方 emotion key 映射：全部落在官方/受控资源 key 集合内。
    const std::string_view kAllowedKeys[] = {
        "boot", "connecting", "error", "happy", "idle", "listening", "provisioning", "sleepy", "speaking", "thinking",
    };
    const VoiceMood kMoods[] = {
        VoiceMood::kBooting,   VoiceMood::kProvisioning, VoiceMood::kConnecting, VoiceMood::kIdle,
        VoiceMood::kListening, VoiceMood::kNeutral,      VoiceMood::kHappy,      VoiceMood::kSad,
        VoiceMood::kThinking,  VoiceMood::kSurprised,    VoiceMood::kSpeaking,   VoiceMood::kCancelled,
        VoiceMood::kAngry,
    };
    for (VoiceMood mood : kMoods) {
        const std::string_view key = EmotionKeyForMood(mood);
        bool allowed = false;
        for (const std::string_view k : kAllowedKeys) {
            if (k == key) {
                allowed = true;
                break;
            }
        }
        Check(allowed, "每个 VoiceMood 的 emotion key 必须属于官方受控资源集合");
    }
    Check(EmotionKeyForMood(VoiceMood::kNeutral) == "idle",
          "待机表情必须映射到官方 idle（VoiceLife manifest 无 neutral.gif）");
    Check(
        EmotionKeyForMood(VoiceMood::kSpeaking) == "speaking" && EmotionKeyForMood(VoiceMood::kThinking) == "thinking",
        "speaking/thinking 必须直映官方同名表情");
    Check(EmotionKeyForMood(VoiceMood::kListening) == "listening" &&
              EmotionKeyForMood(VoiceMood::kConnecting) == "connecting" &&
              EmotionKeyForMood(VoiceMood::kBooting) == "boot",
          "会话可见语义必须映射到对应官方 SparkBot 动画，而非压成 thinking");

    // SPI 逻辑序号：1/2/3 合法（映射到 SDK 的 SPI1/2/3_HOST 符号在 Adapter
    // 内完成，禁止跨 SDK 版本硬编码枚举整数值，防裸值回归）。
    Check(IsValidLogicalSpiHost(1) && IsValidLogicalSpiHost(2) && IsValidLogicalSpiHost(3), "逻辑 SPI 1/2/3 必须合法");
    Check(!IsValidLogicalSpiHost(0) && !IsValidLogicalSpiHost(4), "越界 SPI 序号必须拒绝");

    // 半高布局契约：当前从上半区观察，但不贴左右边缘；三个产品槽位
    // 必须在半高视口内，正文明确采用单行横向循环滚动。
    const auto layout = DefaultSparkBotDisplayLayout();
    Check(layout.viewport_y == 10 && layout.viewport_height == 120, "当前观察布局必须下移并占用可调的上半屏视口");
    Check(layout.viewport_y + layout.viewport_height <= 240, "下移后的产品视口仍必须落在物理屏范围内");
    Check(layout.horizontal_inset >= 8 && 240 - layout.horizontal_inset * 2 < 240, "产品视口必须保留左右安全边距");
    Check(layout.status_top + layout.status_height <= layout.emoji_top, "状态栏和表情舞台不能重叠");
    Check(layout.emoji_top + layout.emoji_size <= layout.content_top, "表情舞台和正文栏不能重叠");
    Check(layout.content_top + layout.content_height <= layout.viewport_height, "正文栏必须落在半高视口内");
    Check(layout.icon_font_size < 20 && layout.emoji_size >= 64 && layout.emoji_size < 96,
          "顶部图标应缩小，中央表情舞台应放大到可辨识尺寸");
    Check(layout.content_scroll_mode == decltype(layout.content_scroll_mode)::kHorizontal,
          "正文栏必须使用单向横向滚动，避免整句重复");
    Check(IsCommonLinxEmojiKey("happy") && IsCommonLinxEmojiKey("confused") &&
              !IsCommonLinxEmojiKey("unknown"),
          "Linx 常用 emoji key 列表必须固化且拒绝未知值");
    Check(ScrollDurationForOverflow(0) == 0 && ScrollDurationForOverflow(1) >= 3000 &&
              ScrollDurationForOverflow(1000) <= 12000,
          "长句滚动时长必须随溢出宽度稳定限制在上下界内");
    Check(layout.screen_saver_top + layout.screen_saver_size <= layout.viewport_height,
          "待机屏保眼睛画布必须落在半高视口内");
    Check(layout.screen_saver_size >= 120 && layout.screen_saver_top + layout.screen_saver_size <= 120 &&
              layout.screen_saver_idle_timeout_ms == 30000,
          "待机屏保必须放大到满半高视口并默认 30 秒进入");

    DisplaySnapshot standby;
    standby.phase = voicelife::voice::VoiceInteractionState::kStandby;
    standby.role = voicelife::voice::VoiceContentRole::kNone;
    Check(IsIdleScreenSaverEligible(standby), "纯待机快照必须允许进入屏保");
    Check(ShouldEnterIdleScreenSaver(standby, 30000, 30000), "达到空闲阈值必须进入屏保");
    Check(!ShouldEnterIdleScreenSaver(standby, 29999, 30000), "未达到空闲阈值不得进入屏保");
    standby.content_text = "日程提醒";
    standby.role = voicelife::voice::VoiceContentRole::kSystem;
    Check(!IsIdleScreenSaverEligible(standby) && ShouldExitIdleScreenSaver(standby), "系统提醒必须抑制并退出屏保");

    // host 构建不触碰 LVGL：SetupUI/Render 必须返回 kUnavailable。
    SparkBotLvglRenderer renderer;
    Check(renderer.SetupUI().code == ErrorCode::kUnavailable, "host 构建 SetupUI 必须返回 kUnavailable（不触碰硬件）");
    Check(renderer.Render(DisplaySnapshot{}).code == ErrorCode::kUnavailable,
          "host 构建 Render 必须返回 kUnavailable（不触碰硬件）");

    return 0;
}
