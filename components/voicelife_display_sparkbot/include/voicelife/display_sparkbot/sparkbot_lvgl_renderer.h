#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include "voicelife/contracts/status.h"
#include "voicelife/display_sparkbot/sparkbot_screen_saver.h"
#include "voicelife/voice/display_snapshot.h"

namespace voicelife::display_sparkbot {

/**
 * @brief 当前 SparkBot 外壳观察用的产品布局参数。
 *
 * 这些参数只约束产品 UI 的安全视口，不改变 ST7789/LVGL 的 240x240
 * 控制器坐标。当前 viewport_y=10 用于避开外壳上沿；实物边界确认后，
 * 后续只需调整 viewport_y 或尺寸，不必重写面板初始化。
 */
struct SparkBotDisplayLayout {
    /** @brief 正文栏的滚动策略。 */
    enum class ContentScrollMode : uint8_t {
        /** 显式从零位向左移动，避免 LVGL 往返/循环模式。 */
        kExplicitLeft,
    };

    // SparkBot 外壳会遮挡物理屏最上沿，产品视口整体下移少量保留安全边距。
    uint16_t viewport_y = 10;
    uint16_t viewport_height = 120;
    uint16_t horizontal_inset = 12;
    uint16_t top_bar_height = 16;
    uint16_t status_top = 16;
    uint16_t status_height = 18;
    uint16_t emoji_top = 34;
    uint16_t emoji_size = 68;
    uint16_t content_top = 102;
    uint16_t content_height = 18;
    uint16_t content_inset = 10;
    // 屏保独占半高产品视口；GIF 自带黑边，放大到满高仍保留左右安全边距。
    uint16_t screen_saver_top = 0;
    uint16_t screen_saver_size = 120;
    uint16_t icon_font_size = 14;
    uint16_t emoji_scale = 181;
    uint32_t screen_saver_idle_timeout_ms = 30000;
    uint32_t content_scroll_duration_ms = 6000;
    ContentScrollMode content_scroll_mode = ContentScrollMode::kExplicitLeft;
};

/**
 * @brief 返回当前上半区观察布局；不代表已完成实板可视区测量。
 * @return 当前半高观察布局参数。
 */
[[nodiscard]] constexpr SparkBotDisplayLayout DefaultSparkBotDisplayLayout() { return {}; }

/**
 * @brief 根据正文溢出宽度计算稳定的单向滚动时长。
 *
 * 短句不滚动；长句按固定像素速度滚动，并限制上下界，避免字幕忽快忽慢。
 * @param overflow_width 正文超出可视区域的像素宽度。
 * @return 滚动时长（毫秒）；不溢出时为 0。
 */
[[nodiscard]] constexpr uint32_t ScrollDurationForOverflow(uint32_t overflow_width) {
    constexpr uint32_t kMinMs = 3000;
    constexpr uint32_t kMaxMs = 12000;
    const uint32_t scaled = overflow_width > kMaxMs / 35 ? kMaxMs : overflow_width * 35;
    return overflow_width == 0 ? 0 : (scaled < kMinMs ? kMinMs : scaled);
}

/**
 * @brief 按 UTF-8 字符数和像素距离估算与语音相近的单向滚动时长。
 * @param overflow_width 正文超出可视区域的像素宽度。
 * @param text 当前字幕文本。
 * @return 估算出的单向滚动时长（毫秒）。
 */
[[nodiscard]] uint32_t ScrollDurationForSubtitle(uint32_t overflow_width, std::string_view text);

/**
 * @brief 计算单向滚动终点，始终为左移的负溢出宽度。
 * @param overflow_width 正文超出可视区域的像素宽度。
 * @return 左移终点偏移量。
 */
[[nodiscard]] constexpr int32_t ScrollEndOffset(uint32_t overflow_width) {
    return overflow_width > 0x7FFFFFFFU ? -0x7FFFFFFF : -static_cast<int32_t>(overflow_width);
}

/** @brief Linx 文档列出的常用情感/动作 key，随应用固化到固件。 */
inline constexpr std::array<std::string_view, 21> kCommonLinxEmojiKeys = {
    "neutral", "loving",    "happy",    "embarrassed", "laughing", "surprised", "funny",
    "shocked", "sad",       "thinking", "angry",       "winking",  "crying",    "cool",
    "relaxed", "delicious", "kissy",    "confident",   "sleepy",   "silly",     "confused",
};

/**
 * @brief 仅允许协议文档定义的 emotion key 进入显示资源映射。
 * @param key 待校验的 Linx emotion key。
 * @return key 是否处于受控常用列表。
 */
[[nodiscard]] constexpr bool IsCommonLinxEmojiKey(std::string_view key) {
    return std::find(kCommonLinxEmojiKeys.begin(), kCommonLinxEmojiKeys.end(), key) != kCommonLinxEmojiKeys.end();
}

/**
 * @brief 返回快照中的受控 Linx key，未知值回退到 VoiceMood。
 * @param snapshot 当前显示语义快照。
 * @return 可映射到显示资源的 emotion key。
 */
[[nodiscard]] std::string_view EmotionKeyForSnapshot(const voicelife::voice::DisplaySnapshot& snapshot);

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
 * @brief SparkBot 官方简单模式 LVGL 渲染器（半高视口适配）。
 *
 * 移植来源：xiaozhi-esp32@37d1aee main/display/lcd_display.cc 的
 * SetupUI 简单模式（顶部状态栏、中央 emoji 舞台、底部消息栏）与官方
 * dark 主题颜色。控制器仍是 240x240；产品对象统一挂在可裁剪的半高
 * 视口下，消息栏使用单行单向横向滚动以节省垂直空间。
 *
 * 中央 emotion 优先使用 assets 分区的 GIF，GIF 不可用时回退到官方字形
 * （xiaozhi-fonts 的 noto_emoji / material_symbols）。
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
    [[maybe_unused]] void* container_ = nullptr;
    /** @brief 官方顶部状态栏。 */
    [[maybe_unused]] void* top_bar_ = nullptr;
    /** @brief 官方顶部网络图标。 */
    [[maybe_unused]] void* network_label_ = nullptr;
    /** @brief 官方顶部音量状态图标。 */
    [[maybe_unused]] void* mute_label_ = nullptr;
    /** @brief 官方顶部电池状态图标。 */
    [[maybe_unused]] void* battery_label_ = nullptr;
    /** @brief 中央 emoji 舞台。 */
    [[maybe_unused]] void* emoji_box_ = nullptr;
    /** @brief 字形 fallback 标签（仅 ESP 构建使用）。 */
    [[maybe_unused]] void* emoji_label_ = nullptr;
    /** @brief emoji 图片节点（GIF 播放目标）。 */
    [[maybe_unused]] void* emoji_image_ = nullptr;
    /** @brief 待机屏保眼睛 GIF 图片节点。 */
    [[maybe_unused]] void* screen_saver_image_ = nullptr;
    /** @brief 状态栏标签。 */
    [[maybe_unused]] void* status_label_ = nullptr;
    /** @brief 底部消息栏容器。 */
    [[maybe_unused]] void* bottom_bar_ = nullptr;
    /** @brief 底部消息标签。 */
    [[maybe_unused]] void* chat_message_label_ = nullptr;
    /** @brief emoji GIF 资源加载器（官方 assets 分区格式）。 */
    [[maybe_unused]] class SparkBotEmojiAssets* emoji_assets_ = nullptr;
    /** @brief mmap common 14px 中文字体经 cbin_font_create 创建的 LVGL 字体。 */
    [[maybe_unused]] void* common_text_font_ = nullptr;
    /** @brief 当前 GIF 播放控制器（LvglGif*）。 */
    [[maybe_unused]] void* gif_controller_ = nullptr;
    /** @brief 当前待机屏保 GIF 播放控制器（LvglGif*）。 */
    [[maybe_unused]] void* screen_saver_gif_controller_ = nullptr;
    /** @brief 待机屏保空闲计时器（lv_timer_t*）。 */
    [[maybe_unused]] void* screen_saver_timer_ = nullptr;
    /** @brief assets 分区是否已成功初始化。 */
    [[maybe_unused]] bool assets_ready_ = false;
    /** @brief 当前 emotion key（同状态不重建 GIF/字形）。 */
    [[maybe_unused]] std::string current_emotion_;
    /** @brief 上一次进入 Renderer 的快照身份，用于屏保事件退出。 */
    [[maybe_unused]] uint64_t last_snapshot_generation_ = 0;
    [[maybe_unused]] uint64_t last_snapshot_revision_ = 0;
    [[maybe_unused]] bool has_snapshot_ = false;
    /** @brief 当前快照是否满足屏保进入条件。 */
    [[maybe_unused]] bool screen_saver_eligible_ = false;
    /** @brief 待机空闲计时起点（LVGL tick）。 */
    [[maybe_unused]] uint32_t standby_idle_started_ms_ = 0;
    [[maybe_unused]] bool standby_idle_tracking_ = false;
    /** @brief 是否已进入黑底动态眼睛屏保。 */
    [[maybe_unused]] bool screen_saver_active_ = false;
    /** @brief 是否已进入 IMU 晕眩屏保；与普通待机屏保共用同一视口。 */
    [[maybe_unused]] bool dizzy_screen_saver_active_ = false;
    /** @brief 已提交给 LVGL 的正文，避免每次快照刷新重启动画。 */
    [[maybe_unused]] std::string rendered_content_text_;

#ifdef ESP_PLATFORM
    void EnterIdleScreenSaver();
    void ExitIdleScreenSaver();
    void EnterDizzyScreenSaver();
    void ExitDizzyScreenSaver();
    bool StartScreenSaverGif(std::string_view asset_id);
    bool StartIdleScreenSaverGif();
    void StopScreenSaverGif();
    void ScreenSaverTimerTick();
    void SetNormalUiVisible(bool visible);
#endif
};

}  // namespace voicelife::display_sparkbot
