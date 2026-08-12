#pragma once

#ifdef ESP_PLATFORM

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "esp_timer.h"
#include "voicelife/voice/voice_interaction_controller.h"

namespace voicelife::runtime {

/** @brief VoiceLife PCB 的 SSD1306 会话状态投影与短暂显示覆盖层。 */
class VoiceLifePcbPresentation final {
   public:
    /** @brief 将已接受的会话状态机事件投影为 OLED 快照。 */
    void ApplyInteraction(voice::VoiceInteractionState state, voice::VoiceInteractionEvent event, bool show_wake_ack,
                          std::string_view user_text);
    /** @brief 显示来自服务端的助手文本，并按 UTF-8 码点管理滚动。 */
    void ShowAssistantText(std::string_view text);
    /** @brief 停止当前长文本滚动。 */
    void StopScroll();
    /** @brief 清空内容栏并提交当前快照。 */
    void ClearContent();
    /** @brief 显示短暂音量覆盖层，到期后恢复最近会话快照。 */
    void ShowVolume(int volume);
    /** @brief 当待机原子条件成立时，显示时钟或空闲状态。 */
    void ShowStandby();
    /** @brief 显示会话收尾提示，不修改会话状态。 */
    void ShowFarewell();

   private:
    static void VolumeOverlayEntry(void* context);
    static void ScrollEntry(void* context);
    static std::size_t CountCodepoints(std::string_view text);
    static std::string_view PhaseStatusText(voice::VoiceInteractionState state);
    static voice::VoiceMood PhaseMood(voice::VoiceInteractionState state);
    void Commit();

    voice::DisplaySnapshot snapshot_;
    std::uint64_t last_rendered_revision_ = 0;
    std::size_t scroll_offset_ = 0;
    std::string scroll_content_;
    esp_timer_handle_t scroll_timer_ = nullptr;
    esp_timer_handle_t volume_overlay_timer_ = nullptr;
};

}  // namespace voicelife::runtime

#endif
