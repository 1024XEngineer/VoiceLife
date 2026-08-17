#include "voicelife/display_esp/ssd1306_presentation_adapter.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>

#include "voicelife/display_esp/ssd1306_status_display.h"

#ifdef ESP_PLATFORM
#include <esp_timer.h>
#endif

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

constexpr std::size_t kContentLinePixels = 128 - 20;

std::size_t Utf8CodepointBytes(std::uint8_t first_byte) {
    if ((first_byte & 0x80) == 0) return 1;
    if ((first_byte & 0xe0) == 0xc0) return 2;
    if ((first_byte & 0xf0) == 0xe0) return 3;
    if ((first_byte & 0xf8) == 0xf0) return 4;
    return 1;
}

#ifdef ESP_PLATFORM
constexpr const char* kTag = "ssd1306_adapter";
/** @brief 滚动可见窗口宽度（字符数，旧行为：末尾留 6 字符）。 */
constexpr std::size_t kScrollWindow = 6;
/** @brief 滚动步进周期（旧行为：400ms/字符）。 */
constexpr uint64_t kScrollPeriodUs = 400 * 1000ULL;

/** @brief UTF-8 码点计数（滚动窗口按字符而非字节，与旧行为一致）。 */
std::size_t CountCodepoints(std::string_view text) {
    std::size_t count = 0;
    for (std::size_t i = 0; i < text.size();) {
        const std::uint8_t b = static_cast<std::uint8_t>(text[i]);
        i += std::min(Utf8CodepointBytes(b), text.size() - i);
        ++count;
    }
    return count;
}
#endif

/** @brief 显示模型表情到旧点阵渲染器 mood 键的映射（与旧行为一致）。 */
[[maybe_unused]] std::string MoodKey(voicelife::voice::VoiceMood mood) {
    switch (mood) {
        case voicelife::voice::VoiceMood::kBooting:
        case voicelife::voice::VoiceMood::kConnecting:
            return "thinking";
        case voicelife::voice::VoiceMood::kProvisioning:
        case voicelife::voice::VoiceMood::kIdle:
            return "neutral";
        case voicelife::voice::VoiceMood::kListening:
            return "surprised";
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
        case voicelife::voice::VoiceMood::kCancelled:
            return "happy";
        case voicelife::voice::VoiceMood::kAngry:
            return "angry";
        default:
            return "neutral";
    }
}

}  // namespace

bool Ssd1306ContentFitsLine(std::string_view content) {
    std::size_t pixels = 0;
    for (std::size_t index = 0; index < content.size();) {
        const std::uint8_t first_byte = static_cast<std::uint8_t>(content[index]);
        const std::size_t advance = (first_byte & 0x80) == 0 ? 9 : 17;
        if (pixels + advance > kContentLinePixels) return false;
        pixels += advance;
        index += std::min(Utf8CodepointBytes(first_byte), content.size() - index);
    }
    return true;
}

Ssd1306PresentationAdapter::~Ssd1306PresentationAdapter() {
#ifdef ESP_PLATFORM
    if (scroll_timer_ != nullptr) {
        esp_timer_stop(static_cast<esp_timer_handle_t>(scroll_timer_));
        esp_timer_delete(static_cast<esp_timer_handle_t>(scroll_timer_));
        scroll_timer_ = nullptr;
    }
#endif
}

const voicelife::voice::DisplayCapabilities& Ssd1306PresentationAdapter::capabilities() const {
    return kSsd1306Capabilities;
}

voicelife::Status Ssd1306PresentationAdapter::Start() { return initialize_(); }

voicelife::Status Ssd1306PresentationAdapter::Render(const voicelife::voice::DisplaySnapshot& snapshot) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    last_snapshot_ = snapshot;
#ifdef ESP_PLATFORM
    const voicelife::Status status =
        display_esp::SetEmotion(MoodKey(snapshot.mood), snapshot.status_text, snapshot.content_text, 0);
    RestartScrollTimer(snapshot.content_text);
    return status;
#else
    (void)snapshot;
    return voicelife::Status::Ok();  // 主机契约测试不触碰硬件
#endif
}

void Ssd1306PresentationAdapter::RestartScrollTimer(const std::string& content) {
#ifdef ESP_PLATFORM
    scroll_offset_ = 0;
    if (scroll_timer_ != nullptr) {
        esp_timer_stop(static_cast<esp_timer_handle_t>(scroll_timer_));
    } else {
        esp_timer_create_args_t args = {};
        args.callback = &Ssd1306PresentationAdapter::ScrollEntry;
        args.arg = this;
        args.name = "voicelife_ssd1306_scroll";
        esp_timer_handle_t timer = nullptr;
        if (esp_timer_create(&args, &timer) != ESP_OK) {
            return;
        }
        scroll_timer_ = timer;
    }
    if (!Ssd1306ContentFitsLine(content)) {
        esp_timer_start_periodic(static_cast<esp_timer_handle_t>(scroll_timer_), kScrollPeriodUs);
    }
#else
    (void)content;
#endif
}

void Ssd1306PresentationAdapter::ScrollEntry(void* arg) {
#ifdef ESP_PLATFORM
    auto* self = static_cast<Ssd1306PresentationAdapter*>(arg);
    std::lock_guard<std::mutex> lock(self->state_mutex_);
    if (Ssd1306ContentFitsLine(self->last_snapshot_.content_text)) {
        esp_timer_stop(static_cast<esp_timer_handle_t>(self->scroll_timer_));
        return;
    }
    const std::size_t codepoints = CountCodepoints(self->last_snapshot_.content_text);
    ++self->scroll_offset_;
    // 末尾留 6 字符可见窗口后停止滚动（按码点，不用 UTF-8 字节数）。
    if (self->scroll_offset_ + kScrollWindow >= codepoints) {
        self->scroll_offset_ = codepoints - kScrollWindow;
        esp_timer_stop(static_cast<esp_timer_handle_t>(self->scroll_timer_));
    }
    (void)display_esp::SetEmotion(MoodKey(self->last_snapshot_.mood), self->last_snapshot_.status_text,
                                  self->last_snapshot_.content_text, self->scroll_offset_);
#else
    (void)arg;
#endif
}

}  // namespace voicelife::display_esp
