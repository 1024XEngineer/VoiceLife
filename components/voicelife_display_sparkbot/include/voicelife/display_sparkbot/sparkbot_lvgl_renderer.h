#pragma once

#include <string_view>

#include "voicelife/contracts/status.h"
#include "voicelife/voice/display_snapshot.h"

namespace voicelife::display_sparkbot {

/**
 * @brief 显示模型表情到官方 SparkBot emotion key 的映射。
 *
 * 官方 emotion key 集合（xiaozhi-esp32@37d1aee 的 emoji 目录）为
 * boot/connecting/error/happy/idle/listening/provisioning/sleepy/
 * speaking/thinking；VoiceLife 的 VoiceMood 没有一一对应的官方表情
 * （官方无 sad/surprised/angry，VoiceLife manifest 无 neutral.gif），
 * 因此按视觉语义就近映射，资源均来自受控资源清单。
 * @param mood 显示模型表情。
 * @return 官方 emotion key（空串表示无对应）。
 */
[[nodiscard]] std::string_view EmotionKeyForMood(voicelife::voice::VoiceMood mood);

/**
 * @brief SparkBot 官方简单模式 LVGL 渲染器（官方移植骨架）。
 *
 * 移植来源：xiaozhi-esp32@37d1aee main/display/lcd_display.cc 的
 * SetupUI 简单模式（中央 96x96 emoji 舞台、顶部状态栏、底部 224x56
 * 消息栏）与官方 dark 主题颜色。布局、主题、字体、字号、颜色、标点、
 * 文本换行与刷新节奏以官方为准，不得自行重新设计 UI。
 *
 * 本阶段 emoji 使用官方字形 fallback（xiaozhi-fonts 的 noto_emoji /
 * material_symbols）；assets 分区的 GIF 资源加载在后续阶段接入。
 * host 构建不触碰 LVGL，SetupUI/Render 返回 kUnavailable。
 */
class SparkBotLvglRenderer {
   public:
    /** @brief 构造函数。 */
    SparkBotLvglRenderer() = default;
    /** @brief 虚析构函数。 */
    ~SparkBotLvglRenderer();

    /** @brief 禁止拷贝构造。 */
    SparkBotLvglRenderer(const SparkBotLvglRenderer&) = delete;
    /** @brief 禁止拷贝赋值。 */
    SparkBotLvglRenderer& operator=(const SparkBotLvglRenderer&) = delete;

    /**
     * @brief 按官方简单模式布局构建 UI（仅一次）。
     * @return 构建结果。
     */
    [[nodiscard]] voicelife::Status SetupUI();

    /**
     * @brief 渲染一份显示快照（官方状态映射 + emoji GIF/字形 + 文本栏）。
     * @param snapshot 只包含业务语义的显示快照。
     * @return 渲染结果。
     */
    [[nodiscard]] voicelife::Status Render(const voicelife::voice::DisplaySnapshot& snapshot);

   private:
    /** @brief 是否已调用 SetupUI（防止重复构建）。 */
    [[maybe_unused]] bool setup_ui_called_ = false;
    /** @brief LVGL 对象句柄（仅 ESP 构建使用，void* 避免公共头依赖 LVGL）。 */
    [[maybe_unused]] void* emoji_box_ = nullptr;
    /** @brief 字形 fallback 标签（仅 ESP 构建使用）。 */
    [[maybe_unused]] void* emoji_label_ = nullptr;
    /** @brief emoji 图片节点（GIF 播放目标）。 */
    [[maybe_unused]] void* emoji_image_ = nullptr;
    /** @brief 状态栏标签。 */
    [[maybe_unused]] void* status_label_ = nullptr;
    /** @brief 底部消息栏容器。 */
    [[maybe_unused]] void* bottom_bar_ = nullptr;
    /** @brief 底部消息标签。 */
    [[maybe_unused]] void* chat_message_label_ = nullptr;
    /** @brief emoji GIF 资源加载器（官方 assets 分区格式）。 */
    [[maybe_unused]] class SparkBotEmojiAssets* emoji_assets_ = nullptr;
    /** @brief 当前 GIF 播放控制器（LvglGif*）。 */
    [[maybe_unused]] void* gif_controller_ = nullptr;
    /** @brief assets 分区是否已成功初始化。 */
    [[maybe_unused]] bool assets_ready_ = false;
};

}  // namespace voicelife::display_sparkbot
