#include "runtime_presentation.h"

#ifdef ESP_PLATFORM

#include <cstdio>
#include <ctime>

#include "voicelife/display_esp/ssd1306_status_display.h"

namespace voicelife::runtime {
namespace {

constexpr int64_t kVolumeOverlayUs = 1500 * 1000;
constexpr std::size_t kVisibleCodepoints = 6;

}  // namespace

void VoiceLifePcbPresentation::ApplyInteraction(voice::VoiceInteractionState state, voice::VoiceInteractionEvent event,
                                                bool show_wake_ack, std::string_view user_text) {
    snapshot_.phase = state;
    snapshot_.mood = PhaseMood(state);
    const time_t now = time(nullptr);
    if (state == voice::VoiceInteractionState::kStandby && now > 1600000000) {
        std::tm local{};
        localtime_r(&now, &local);
        char clock_text[8] = {};
        std::snprintf(clock_text, sizeof(clock_text), "%02d:%02d", local.tm_hour, local.tm_min);
        snapshot_.status_text = clock_text;
    } else {
        snapshot_.status_text = PhaseStatusText(state);
    }

    if (event == voice::VoiceInteractionEvent::kWakeDetected && state == voice::VoiceInteractionState::kListening &&
        show_wake_ack) {
        snapshot_.content_text = "收到！";
        snapshot_.role = voice::VoiceContentRole::kSystem;
    } else if (event == voice::VoiceInteractionEvent::kEndpointDetected) {
        snapshot_.content_text.clear();
        snapshot_.role = voice::VoiceContentRole::kNone;
    } else if (event == voice::VoiceInteractionEvent::kIntentReceived && !user_text.empty()) {
        snapshot_.content_text = user_text;
        snapshot_.role = voice::VoiceContentRole::kUser;
    } else if (event == voice::VoiceInteractionEvent::kTtsStopped ||
               event == voice::VoiceInteractionEvent::kStandbyReady ||
               event == voice::VoiceInteractionEvent::kBootCompleted) {
        snapshot_.content_text.clear();
        snapshot_.role = voice::VoiceContentRole::kNone;
    }
    ++snapshot_.revision;
    Commit();
}

void VoiceLifePcbPresentation::ShowAssistantText(std::string_view text) {
    if (text.empty()) return;
    snapshot_.content_text = text;
    snapshot_.role = voice::VoiceContentRole::kAssistant;
    snapshot_.status_text = "说话中";
    snapshot_.mood = voice::VoiceMood::kSpeaking;
    scroll_content_ = text;
    scroll_offset_ = 0;
    if (scroll_timer_ == nullptr) {
        esp_timer_create_args_t args = {};
        args.callback = &ScrollEntry;
        args.arg = this;
        args.name = "voicelife_scroll";
        (void)esp_timer_create(&args, &scroll_timer_);
    }
    if (scroll_timer_ != nullptr) {
        (void)esp_timer_stop(scroll_timer_);
        if (CountCodepoints(scroll_content_) > kVisibleCodepoints) {
            (void)esp_timer_start_periodic(scroll_timer_, 400 * 1000ULL);
        }
    }
    ++snapshot_.revision;
    Commit();
}

void VoiceLifePcbPresentation::StopScroll() {
    if (scroll_timer_ != nullptr) (void)esp_timer_stop(scroll_timer_);
}

void VoiceLifePcbPresentation::ClearContent() {
    snapshot_.content_text.clear();
    snapshot_.role = voice::VoiceContentRole::kNone;
    ++snapshot_.revision;
    Commit();
}

void VoiceLifePcbPresentation::ShowVolume(int volume) {
    char text[16] = {};
    std::snprintf(text, sizeof(text), "VOL:%d", volume);
    (void)display_esp::SetEmotion("neutral", "音量", text);
    if (volume_overlay_timer_ == nullptr) {
        esp_timer_create_args_t args = {};
        args.callback = &VolumeOverlayEntry;
        args.arg = this;
        args.name = "voicelife_volume_overlay";
        (void)esp_timer_create(&args, &volume_overlay_timer_);
    }
    if (volume_overlay_timer_ != nullptr) {
        (void)esp_timer_stop(volume_overlay_timer_);
        (void)esp_timer_start_once(volume_overlay_timer_, kVolumeOverlayUs);
    }
}

void VoiceLifePcbPresentation::ShowStandby() {
    snapshot_.phase = voice::VoiceInteractionState::kStandby;
    snapshot_.mood = voice::VoiceMood::kNeutral;
    const time_t now = time(nullptr);
    if (now > 1600000000) {
        std::tm local{};
        localtime_r(&now, &local);
        char clock_text[8] = {};
        std::snprintf(clock_text, sizeof(clock_text), "%02d:%02d", local.tm_hour, local.tm_min);
        snapshot_.status_text = clock_text;
    } else {
        snapshot_.status_text = PhaseStatusText(voice::VoiceInteractionState::kStandby);
    }
    snapshot_.content_text.clear();
    snapshot_.role = voice::VoiceContentRole::kNone;
    ++snapshot_.revision;
    Commit();
}

void VoiceLifePcbPresentation::ShowFarewell() { (void)display_esp::SetEmotion("happy", "牛牛走了！", {}); }

void VoiceLifePcbPresentation::VolumeOverlayEntry(void* context) {
    auto* self = static_cast<VoiceLifePcbPresentation*>(context);
    ++self->snapshot_.revision;
    self->Commit();
}

void VoiceLifePcbPresentation::ScrollEntry(void* context) {
    auto* self = static_cast<VoiceLifePcbPresentation*>(context);
    const std::size_t codepoints = CountCodepoints(self->scroll_content_);
    if (codepoints <= kVisibleCodepoints) {
        self->StopScroll();
        return;
    }
    ++self->scroll_offset_;
    if (self->scroll_offset_ + kVisibleCodepoints >= codepoints) {
        self->scroll_offset_ = codepoints - kVisibleCodepoints;
        self->StopScroll();
    }
    ++self->snapshot_.revision;
    self->Commit();
}

std::size_t VoiceLifePcbPresentation::CountCodepoints(std::string_view text) {
    std::size_t count = 0;
    for (std::size_t index = 0; index < text.size();) {
        const uint8_t byte = static_cast<uint8_t>(text[index]);
        std::size_t width = 1;
        if ((byte & 0xe0U) == 0xc0U) width = 2;
        if ((byte & 0xf0U) == 0xe0U) width = 3;
        if ((byte & 0xf8U) == 0xf0U) width = 4;
        index += width;
        ++count;
    }
    return count;
}

std::string_view VoiceLifePcbPresentation::PhaseStatusText(voice::VoiceInteractionState state) {
    switch (state) {
        case voice::VoiceInteractionState::kBooting:
            return "开机";
        case voice::VoiceInteractionState::kStandby:
            return "空闲";
        case voice::VoiceInteractionState::kOpeningCapture:
        case voice::VoiceInteractionState::kListening:
        case voice::VoiceInteractionState::kFinalizing:
            return "聆听中";
        case voice::VoiceInteractionState::kThinking:
            return "处理中";
        case voice::VoiceInteractionState::kSpeaking:
            return "说话中";
        case voice::VoiceInteractionState::kInterrupting:
            return "停止";
        case voice::VoiceInteractionState::kReconnecting:
            return "重连中";
        case voice::VoiceInteractionState::kError:
            return "出错了";
    }
    return "出错了";
}

voice::VoiceMood VoiceLifePcbPresentation::PhaseMood(voice::VoiceInteractionState state) {
    switch (state) {
        case voice::VoiceInteractionState::kListening:
        case voice::VoiceInteractionState::kFinalizing:
        case voice::VoiceInteractionState::kThinking:
        case voice::VoiceInteractionState::kReconnecting:
            return voice::VoiceMood::kThinking;
        case voice::VoiceInteractionState::kSpeaking:
            return voice::VoiceMood::kSpeaking;
        case voice::VoiceInteractionState::kInterrupting:
            return voice::VoiceMood::kSurprised;
        case voice::VoiceInteractionState::kError:
            return voice::VoiceMood::kSad;
        default:
            return voice::VoiceMood::kNeutral;
    }
}

void VoiceLifePcbPresentation::Commit() {
    if (snapshot_.revision == last_rendered_revision_) return;
    last_rendered_revision_ = snapshot_.revision;
    std::string_view mood_key = "neutral";
    switch (snapshot_.mood) {
        case voice::VoiceMood::kHappy:
            mood_key = "happy";
            break;
        case voice::VoiceMood::kSad:
            mood_key = "sad";
            break;
        case voice::VoiceMood::kThinking:
            mood_key = "thinking";
            break;
        case voice::VoiceMood::kSurprised:
            mood_key = "surprised";
            break;
        case voice::VoiceMood::kSpeaking:
            mood_key = "speaking";
            break;
        case voice::VoiceMood::kAngry:
            mood_key = "angry";
            break;
        default:
            break;
    }
    (void)display_esp::SetEmotion(mood_key, snapshot_.status_text, snapshot_.content_text, scroll_offset_);
}

}  // namespace voicelife::runtime

#endif
