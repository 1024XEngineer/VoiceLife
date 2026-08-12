#include "voicelife/display_esp/ssd1306_presentation_adapter.h"

#include <string>

#include "voicelife/display_esp/ssd1306_status_display.h"

namespace voicelife::display_esp {

namespace {

/** @brief 点阵屏能力：文本可用，无图片/动画/预览能力。 */
constexpr voicelife::voice::DisplayCapabilities kSsd1306Capabilities{
    .available = true,
    .text = true,
    .static_image = false,
    .animation = false,
    .preview_image = false,
    .max_frame_bytes = 0,
    .refresh_budget_hz = 0,
};

/** @brief 显示模型表情到旧点阵渲染器 mood 键的映射（与旧行为一致）。 */
[[maybe_unused]] std::string MoodKey(voicelife::voice::VoiceMood mood) {
    switch (mood) {
        case voicelife::voice::VoiceMood::kHappy:
            return "happy";
        case voicelife::voice::VoiceMood::kSad:
            return "sad";
        case voicelife::voice::VoiceMood::kThinking:
            return "thinking";
        case voicelife::voice::VoiceMood::kSurprised:
            return "surprised";
        case voicelife::voice::VoiceMood::kSpeaking:
            return "speaking";
        case voicelife::voice::VoiceMood::kAngry:
            return "angry";
        default:
            return "neutral";
    }
}

}  // namespace

const voicelife::voice::DisplayCapabilities& Ssd1306PresentationAdapter::capabilities() const {
    return kSsd1306Capabilities;
}

voicelife::Status Ssd1306PresentationAdapter::Render(const voicelife::voice::DisplaySnapshot& snapshot) {
#ifdef ESP_PLATFORM
    // 长文本滚动由显示任务维护 scroll_offset（当前骨架固定 0，接入显示任务
    // 时沿用旧滚动语义）。
    return display_esp::SetEmotion(MoodKey(snapshot.mood), snapshot.status_text, snapshot.content_text, 0);
#else
    (void)snapshot;
    return voicelife::Status::Ok();  // 主机契约测试不触碰硬件
#endif
}

voicelife::Status Ssd1306PresentationAdapter::Submit(voicelife::voice::PresentationCommand /*command*/) {
    return voicelife::Status::Error(voicelife::ErrorCode::kUnavailable, "SSD1306 点阵屏不支持图片/动画资源命令");
}

}  // namespace voicelife::display_esp
