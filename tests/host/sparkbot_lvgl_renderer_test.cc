#include "voicelife/display_sparkbot/sparkbot_lvgl_renderer.h"

#include "support/test_support.h"
#include "voicelife/display_sparkbot/sparkbot_lvgl_display.h"
#include "voicelife/voice/display_snapshot.h"

using voicelife::ErrorCode;
using voicelife::test::Check;
using voicelife::voice::DisplaySnapshot;
using voicelife::voice::VoiceMood;

int main() {
    using voicelife::display_sparkbot::EmotionKeyForMood;
    using voicelife::display_sparkbot::IsValidLogicalSpiHost;
    using voicelife::display_sparkbot::SparkBotLvglRenderer;

    // 官方 emotion key 映射：全部落在官方/受控资源 key 集合内。
    const std::string_view kAllowedKeys[] = {
        "boot", "connecting", "error", "happy", "idle", "listening", "provisioning", "sleepy", "speaking", "thinking",
    };
    const VoiceMood kMoods[] = {
        VoiceMood::kBooting, VoiceMood::kProvisioning, VoiceMood::kConnecting, VoiceMood::kIdle,
        VoiceMood::kListening, VoiceMood::kNeutral, VoiceMood::kHappy, VoiceMood::kSad,
        VoiceMood::kThinking, VoiceMood::kSurprised, VoiceMood::kSpeaking, VoiceMood::kCancelled,
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

    // host 构建不触碰 LVGL：SetupUI/Render 必须返回 kUnavailable。
    SparkBotLvglRenderer renderer;
    Check(renderer.SetupUI().code == ErrorCode::kUnavailable, "host 构建 SetupUI 必须返回 kUnavailable（不触碰硬件）");
    Check(renderer.Render(DisplaySnapshot{}).code == ErrorCode::kUnavailable,
          "host 构建 Render 必须返回 kUnavailable（不触碰硬件）");

    return 0;
}
