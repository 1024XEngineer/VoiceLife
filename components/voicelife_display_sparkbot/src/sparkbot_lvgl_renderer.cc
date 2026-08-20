#include "voicelife/display_sparkbot/sparkbot_lvgl_renderer.h"

#ifdef ESP_PLATFORM
#include <cbin_font.h>
#include <esp_log.h>
#include <lvgl.h>
#include <material_symbols.h>
#include <noto_emoji.h>
#include <sdkconfig.h>

#include "gif/lvgl_gif.h"
#include "voicelife/display_sparkbot/sparkbot_emoji_assets.h"
#endif

// 字体符号由 xiaozhi-fonts 组件提供（全局链接）；LV_FONT_DECLARE 必须位于
// 全局作用域，否则匿名命名空间会把引用变成内部链接导致 undefined reference。
#ifdef ESP_PLATFORM
LV_FONT_DECLARE(font_noto_sans_basic_16_4);
LV_FONT_DECLARE(font_noto_sans_basic_14_1);
LV_FONT_DECLARE(font_material_symbols_14_1);
LV_FONT_DECLARE(font_material_symbols_20_4);
LV_FONT_DECLARE(font_material_symbols_30_4);
LV_FONT_DECLARE(font_noto_emoji_30_4);
#endif

namespace voicelife::display_sparkbot {

namespace {
#ifdef ESP_PLATFORM
constexpr const char* kTag = "sparkbot_renderer";

// 官方 SparkBot 强制 dark 主题颜色（lcd_display.cc InitializeLcdThemes）。
const lv_color_t kBackgroundColor = lv_color_hex(0x000000);
const lv_color_t kTextColor = lv_color_hex(0xFFFFFF);

// 横向滚动只需要一行；保留 UTF-8 字节序列，只把显式换行折叠为空格。
std::string FlattenSubtitleLine(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (const char character : text) {
        result.push_back(character == '\n' || character == '\r' ? ' ' : character);
    }
    return result;
}

bool HasRenderableGlyph(const lv_font_t* font, uint32_t codepoint, uint16_t* advance) {
    if (font == nullptr) {
        return false;
    }
    lv_font_glyph_dsc_t glyph{};
    if (!lv_font_get_glyph_dsc(font, &glyph, codepoint, 0) || glyph.resolved_font == nullptr || glyph.box_w == 0 ||
        glyph.box_h == 0 || glyph.adv_w == 0) {
        return false;
    }
    if (advance != nullptr) {
        *advance = glyph.adv_w;
    }
    // lv_font_get_glyph_bitmap() decodes into an LVGL draw buffer for this
    // font format. SetupUI has no draw buffer, so descriptor resolution is
    // the safe startup-time proof; the renderer obtains the bitmap later in
    // LVGL's normal draw context.
    lv_font_glyph_release_draw_data(&glyph);
    return true;
}
#endif
}  // namespace

std::string_view EmotionKeyForMood(voicelife::voice::VoiceMood mood) {
    // 官方无 sad/surprised/angry 表情，VoiceLife manifest 无 neutral.gif，
    // 按视觉语义就近映射；资源均来自受控资源清单。
    switch (mood) {
        case voicelife::voice::VoiceMood::kBooting:
            return "boot";
        case voicelife::voice::VoiceMood::kProvisioning:
            return "provisioning";
        case voicelife::voice::VoiceMood::kConnecting:
            return "connecting";
        case voicelife::voice::VoiceMood::kIdle:
            return "idle";
        case voicelife::voice::VoiceMood::kListening:
            return "listening";
        case voicelife::voice::VoiceMood::kHappy:
            return "happy";
        case voicelife::voice::VoiceMood::kSad:
            return "error";
        case voicelife::voice::VoiceMood::kThinking:
            return "thinking";
        case voicelife::voice::VoiceMood::kSurprised:
            return "happy";
        case voicelife::voice::VoiceMood::kSpeaking:
            return "speaking";
        case voicelife::voice::VoiceMood::kCancelled:
            return "sleepy";
        case voicelife::voice::VoiceMood::kAngry:
            return "error";
        case voicelife::voice::VoiceMood::kNeutral:
        default:
            return "idle";
    }
}

SparkBotLvglRenderer::~SparkBotLvglRenderer() {
#ifdef ESP_PLATFORM
    if (screen_saver_timer_ != nullptr) {
        lv_timer_del(static_cast<lv_timer_t*>(screen_saver_timer_));
        screen_saver_timer_ = nullptr;
    }
    StopIdleScreenSaverGif();
    if (gif_controller_ != nullptr) {
        auto* gif = static_cast<LvglGif*>(gif_controller_);
        gif->Stop();
        delete gif;
        gif_controller_ = nullptr;
    }
    if (common_text_font_ != nullptr) {
        cbin_font_delete(static_cast<lv_font_t*>(common_text_font_));
        common_text_font_ = nullptr;
    }
    delete static_cast<SparkBotEmojiAssets*>(emoji_assets_);
    emoji_assets_ = nullptr;
#endif
}

voicelife::Status SparkBotLvglRenderer::SetupUI() {
#ifdef ESP_PLATFORM
    if (setup_ui_called_) {
        ESP_LOGW(kTag, "SetupUI() 重复调用，跳过");
        return voicelife::Status::Ok();
    }
    setup_ui_called_ = true;

    constexpr SparkBotDisplayLayout layout = DefaultSparkBotDisplayLayout();
    const lv_coord_t viewport_width = LV_HOR_RES - static_cast<lv_coord_t>(layout.horizontal_inset * 2);

    // 官方简单模式的 dark 主题保留，但产品对象只占当前可调的半高视口。
    // common CBIN 使用同源 14px/1bpp 中文字体，保证小字仍能覆盖中文；basic
    // 仅作 fallback，顶部 Material Symbols 图标只缩小字号，不改变语义。
    emoji_assets_ = new SparkBotEmojiAssets();
    assets_ready_ = emoji_assets_->Initialize().ok();
    // 半高视口的状态和正文使用 14px common 字形；assets 不可用时回退
    // 到 basic 字形，保持启动路径可用。
    const lv_font_t* text_font = &font_noto_sans_basic_14_1;
    if (assets_ready_) {
        const auto common_font_asset = emoji_assets_->LoadCommonTextFont();
        if (common_font_asset.ok() && common_font_asset.value.has_value()) {
            auto* common_font =
                cbin_font_create(const_cast<uint8_t*>(static_cast<const uint8_t*>(common_font_asset.value->data)));
            if (common_font != nullptr && common_font->line_height == 16 && common_font->base_line == 2 &&
                common_font->dsc != nullptr && static_cast<const lv_font_fmt_txt_dsc_t*>(common_font->dsc)->bpp == 1) {
                common_font->fallback = &font_noto_sans_basic_14_1;
                common_text_font_ = common_font;
                text_font = common_font;
                ESP_LOGI(kTag, "SPARKBOT_COMMON_FONT_READY size=14 bpp=1 line_height=16 fallback=basic14");
            } else {
                if (common_font != nullptr) {
                    cbin_font_delete(common_font);
                }
                ESP_LOGW(kTag, "common 14px 字体元数据不符合预期规格，回退 basic");
            }
        }
    }
    if (!assets_ready_) {
        ESP_LOGW(kTag, "assets 分区不可用，emoji 与文本回退内置字形");
    }
    uint16_t kai_advance = 0;
    uint16_t xian_advance = 0;
    const bool kai_ok = HasRenderableGlyph(text_font, 0x5F00, &kai_advance);    // 开
    const bool xian_ok = HasRenderableGlyph(text_font, 0x95F2, &xian_advance);  // 闲
    ESP_LOGI(kTag, "SPARKBOT_TEXT_GLYPH_CHECK kai=%d kai_adv=%u xian=%d xian_adv=%u common_font=%d", kai_ok,
             static_cast<unsigned>(kai_advance), xian_ok, static_cast<unsigned>(xian_advance),
             common_text_font_ != nullptr);

    auto* screen = lv_screen_active();
    lv_obj_set_style_text_font(screen, text_font, 0);
    lv_obj_set_style_text_color(screen, kTextColor, 0);
    lv_obj_set_style_bg_color(screen, kBackgroundColor, 0);

    // 控制器仍为 240x240；这个根节点是产品可视区，左右留出安全边距，
    // 后续只需调整 layout.viewport_y 即可向下平移整组内容。
    auto* container = lv_obj_create(screen);
    lv_obj_set_size(container, viewport_width, layout.viewport_height);
    lv_obj_set_style_radius(container, 0, 0);
    lv_obj_set_style_pad_all(container, 0, 0);
    lv_obj_set_style_border_width(container, 0, 0);
    lv_obj_set_style_bg_color(container, kBackgroundColor, 0);
    lv_obj_clear_flag(container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(container, LV_DIR_NONE);
    lv_obj_align(container, LV_ALIGN_TOP_MID, 0, layout.viewport_y);
    container_ = container;

    // 中央 emoji 舞台：保留左右安全边距，同时放大到 68px；GIF 源仍可保持
    // 官方 96x96，scale=181 对应约 71px 的实际绘制尺寸。
    auto* emoji_box = lv_obj_create(container);
    lv_obj_set_size(emoji_box, layout.emoji_size, layout.emoji_size);
    lv_obj_set_style_bg_opa(emoji_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(emoji_box, 0, 0);
    lv_obj_set_style_border_width(emoji_box, 0, 0);
    lv_obj_align(emoji_box, LV_ALIGN_TOP_MID, 0, layout.emoji_top);
    emoji_box_ = emoji_box;

    // 字形 fallback 标签（官方 emoji_label_，默认 robot 字形）。
    auto* emoji_label = lv_label_create(emoji_box);
    lv_obj_set_size(emoji_label, layout.emoji_size, layout.emoji_size);
    lv_obj_set_style_text_font(emoji_label, &font_material_symbols_30_4, 0);
    lv_obj_set_style_text_color(emoji_label, kTextColor, 0);
    lv_label_set_long_mode(emoji_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(emoji_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(emoji_label, MATERIAL_SYMBOLS_ROBOT_2);
    lv_obj_center(emoji_label);
    emoji_label_ = emoji_label;

    // emoji 图片节点（后续 GIF 资源接入后使用；当前隐藏）。
    auto* emoji_image = lv_img_create(emoji_box);
    lv_obj_set_size(emoji_image, layout.emoji_size, layout.emoji_size);
    lv_image_set_scale(emoji_image, layout.emoji_scale);
    lv_image_set_inner_align(emoji_image, LV_IMAGE_ALIGN_CENTER);
    lv_obj_center(emoji_image);
    lv_obj_add_flag(emoji_image, LV_OBJ_FLAG_HIDDEN);
    emoji_image_ = emoji_image;

    // 待机屏保占满当前半高产品画布；GIF 自带黑边，放大 96x96 源图后
    // 眼睛仍不会贴到左右安全边缘。后续调整 viewport_y 时只需平移这一层。
    auto* screen_saver_image = lv_img_create(container);
    lv_obj_set_size(screen_saver_image, layout.screen_saver_size, layout.screen_saver_size);
    lv_image_set_scale(screen_saver_image, static_cast<uint32_t>(layout.screen_saver_size) * 256U / 96U);
    lv_image_set_inner_align(screen_saver_image, LV_IMAGE_ALIGN_CENTER);
    lv_obj_align(screen_saver_image, LV_ALIGN_TOP_MID, 0, layout.screen_saver_top);
    lv_obj_add_flag(screen_saver_image, LV_OBJ_FLAG_HIDDEN);
    screen_saver_image_ = screen_saver_image;

    // 官方 top_bar：左网络图标，右侧音量、电池和已接入能力图标。
    auto* top_bar = lv_obj_create(container);
    lv_obj_set_size(top_bar, viewport_width, layout.top_bar_height);
    lv_obj_set_style_radius(top_bar, 0, 0);
    lv_obj_set_style_bg_opa(top_bar, LV_OPA_50, 0);
    lv_obj_set_style_bg_color(top_bar, kBackgroundColor, 0);
    lv_obj_set_style_border_width(top_bar, 0, 0);
    lv_obj_set_style_pad_all(top_bar, 0, 0);
    lv_obj_set_style_pad_top(top_bar, 1, 0);
    lv_obj_set_style_pad_bottom(top_bar, 1, 0);
    lv_obj_set_style_pad_left(top_bar, 2, 0);
    lv_obj_set_style_pad_right(top_bar, 2, 0);
    lv_obj_set_flex_flow(top_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top_bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(top_bar, LV_SCROLLBAR_MODE_OFF);
    lv_obj_align(top_bar, LV_ALIGN_TOP_MID, 0, 0);
    top_bar_ = top_bar;

    // Wi-Fi 图标由 DisplaySnapshot 的受控网络语义驱动；Renderer 不读取
    // Wi-Fi 驱动或板级状态，保持 Runtime/Adapter 边界。
    auto* network_label = lv_label_create(top_bar);
    lv_label_set_text(network_label, "");
    lv_obj_set_style_text_font(network_label, &font_material_symbols_14_1, 0);
    lv_obj_set_style_text_color(network_label, kTextColor, 0);
    network_label_ = network_label;

    auto* right_icons = lv_obj_create(top_bar);
    lv_obj_set_size(right_icons, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(right_icons, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right_icons, 0, 0);
    lv_obj_set_style_pad_all(right_icons, 0, 0);
    lv_obj_set_flex_flow(right_icons, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right_icons, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    auto* mute_label = lv_label_create(right_icons);
    lv_label_set_text(mute_label, "");
    lv_obj_set_style_text_font(mute_label, &font_material_symbols_14_1, 0);
    lv_obj_set_style_text_color(mute_label, kTextColor, 0);
    mute_label_ = mute_label;

    auto* battery_label = lv_label_create(right_icons);
    lv_label_set_text(battery_label, "");
    lv_obj_set_style_text_font(battery_label, &font_material_symbols_14_1, 0);
    lv_obj_set_style_text_color(battery_label, kTextColor, 0);
    lv_obj_set_style_margin_left(battery_label, 1, 0);
    battery_label_ = battery_label;

    // Material Symbols 子集不包含 ASCII space；将已接入的能力 glyph 拆为独立
    // label，以 margin 控制官方同等间距，避免空格落到缺字框而显示乱码。
    auto* microphone_label = lv_label_create(right_icons);
    lv_label_set_text(microphone_label, MATERIAL_SYMBOLS_MIC);
    lv_obj_set_style_text_font(microphone_label, &font_material_symbols_14_1, 0);
    lv_obj_set_style_text_color(microphone_label, lv_color_hex(0x6DD8E8), 0);
    lv_obj_set_style_margin_left(microphone_label, 1, 0);

    // 状态栏：仍在图标下方，但宽度遵守左右安全边距。
    auto* status_bar = lv_obj_create(container);
    lv_obj_set_size(status_bar, viewport_width - 16, layout.status_height);
    lv_obj_set_style_radius(status_bar, 0, 0);
    lv_obj_set_style_bg_opa(status_bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(status_bar, 0, 0);
    lv_obj_set_style_pad_all(status_bar, 0, 0);
    // 状态栏采用绝对布局；14px 字体的行框固定为半高布局预留的 18px。
    lv_obj_set_style_pad_top(status_bar, 0, 0);
    lv_obj_set_style_pad_bottom(status_bar, 0, 0);
    lv_obj_set_style_layout(status_bar, LV_LAYOUT_NONE, 0);
    // CBIN 字体来自 assets mmap，不能依赖跨层对象的样式继承。状态/消息
    // label 都显式绑定同一个字体，保证常用中文在 SparkBot 上可见。
    lv_obj_set_style_text_font(status_bar, text_font, 0);
    lv_obj_set_style_text_color(status_bar, kTextColor, 0);
    lv_obj_set_style_text_opa(status_bar, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(status_bar, LV_SCROLLBAR_MODE_OFF);
    lv_obj_align(status_bar, LV_ALIGN_TOP_MID, 0, layout.status_top);

    auto* status_label = lv_label_create(status_bar);
    lv_obj_set_width(status_label, viewport_width - 16);
    lv_obj_set_height(status_label, text_font->line_height);
    lv_label_set_long_mode(status_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(status_label, text_font, 0);
    lv_obj_set_style_text_color(status_label, kTextColor, 0);
    lv_obj_set_style_text_opa(status_label, LV_OPA_COVER, 0);
    lv_label_set_text(status_label, "");
    lv_obj_align(status_label, LV_ALIGN_CENTER, 0, 0);
    status_label_ = status_label;

    // 底部消息栏：固定一行，长内容交给 label 做横向循环滚动，避免占用
    // 表情舞台的垂直空间。消息区域自身不滚动，父容器负责裁剪。
    const lv_coord_t message_width = viewport_width - static_cast<lv_coord_t>(layout.content_inset * 2);
    auto* bottom_bar = lv_obj_create(container);
    lv_obj_set_width(bottom_bar, message_width);
    lv_obj_set_height(bottom_bar, layout.content_height);
    lv_obj_set_style_radius(bottom_bar, 0, 0);
    lv_obj_set_style_bg_color(bottom_bar, kBackgroundColor, 0);
    lv_obj_set_style_bg_opa(bottom_bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_font(bottom_bar, text_font, 0);
    lv_obj_set_style_text_color(bottom_bar, kTextColor, 0);
    lv_obj_set_style_text_opa(bottom_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(bottom_bar, 0, 0);
    lv_obj_set_style_border_width(bottom_bar, 0, 0);
    lv_obj_set_scrollbar_mode(bottom_bar, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(bottom_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(bottom_bar, LV_DIR_NONE);
    lv_obj_align(bottom_bar, LV_ALIGN_TOP_MID, 0, layout.content_top);

    auto* chat_message_label = lv_label_create(bottom_bar);
    lv_label_set_text(chat_message_label, "");
    lv_obj_set_width(chat_message_label, message_width);
    lv_label_set_long_mode(chat_message_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_height(chat_message_label, layout.content_height);
    lv_obj_set_style_text_align(chat_message_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(chat_message_label, text_font, 0);
    lv_obj_set_style_text_color(chat_message_label, kTextColor, 0);
    lv_obj_set_style_text_opa(chat_message_label, LV_OPA_COVER, 0);
    lv_obj_set_style_anim_duration(chat_message_label, layout.content_scroll_duration_ms, 0);
    lv_obj_align(chat_message_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(bottom_bar, LV_OBJ_FLAG_HIDDEN);  // 有内容才显示
    bottom_bar_ = bottom_bar;
    chat_message_label_ = chat_message_label;

    auto* screen_saver_timer = lv_timer_create(
        [](lv_timer_t* timer) {
            auto* renderer = static_cast<SparkBotLvglRenderer*>(lv_timer_get_user_data(timer));
            renderer->ScreenSaverTimerTick();
        },
        250, this);
    screen_saver_timer_ = screen_saver_timer;

    return voicelife::Status::Ok();
#else
    (void)0;
    return voicelife::Status::Error(voicelife::ErrorCode::kUnavailable, "主机构建不初始化真实 LVGL UI");
#endif
}

voicelife::Status SparkBotLvglRenderer::Render(const voicelife::voice::DisplaySnapshot& snapshot) {
#ifdef ESP_PLATFORM
    if (!setup_ui_called_) {
        const auto setup = SetupUI();
        if (!setup.ok()) {
            return setup;
        }
    }

    const bool snapshot_changed = !has_snapshot_ || snapshot.generation != last_snapshot_generation_ ||
                                  snapshot.revision != last_snapshot_revision_;
    if (screen_saver_active_ && snapshot_changed) {
        ExitIdleScreenSaver();
    }
    has_snapshot_ = true;
    last_snapshot_generation_ = snapshot.generation;
    last_snapshot_revision_ = snapshot.revision;
    screen_saver_eligible_ = IsIdleScreenSaverEligible(snapshot);
    if (!screen_saver_active_) {
        if (screen_saver_eligible_) {
            if (!standby_idle_tracking_) {
                standby_idle_started_ms_ = lv_tick_get();
                standby_idle_tracking_ = true;
                ESP_LOGI(kTag, "SPARKBOT_IDLE_SCREENSAVER_ARMED=1 timeout_ms=%u",
                         static_cast<unsigned>(DefaultSparkBotDisplayLayout().screen_saver_idle_timeout_ms));
            }
        } else {
            standby_idle_tracking_ = false;
        }
    }
    if (screen_saver_active_) {
        if (!ShouldExitIdleScreenSaver(snapshot)) {
            return voicelife::Status::Ok();
        }
        ExitIdleScreenSaver();
    }

    // 官方 SetEmotion：优先 emoji GIF（assets 分区），失败回退字形。
    // 仅 emotion（mood 映射的 asset）变化时切换 GIF/字形；同状态下只更新
    // 文本，避免状态文本刷新反复重建并重启动画。
    const std::string_view emotion = EmotionKeyForMood(snapshot.mood);
    const bool emotion_changed = emotion != current_emotion_;
    bool using_gif = false;
    if (emotion_changed && gif_controller_ != nullptr) {
        // 和官方 SetEmotion 一样，在切换 source 的同一 LVGL 锁上下文中停掉
        // 并释放旧解码器，避免定时器继续访问已经替换的 image 数据。
        ESP_LOGI(kTag, "SPARKBOT_GIF_REPLACED old=%s new=%.*s", current_emotion_.c_str(),
                 static_cast<int>(emotion.size()), emotion.data());
        auto* old_gif = static_cast<LvglGif*>(gif_controller_);
        old_gif->Stop();
        delete old_gif;
        gif_controller_ = nullptr;
    }
    if (emotion_changed && emoji_assets_ != nullptr && assets_ready_) {
        const auto asset = emoji_assets_->Load(emotion);
        if (asset.ok() && asset.value.has_value() && asset.value->data != nullptr && asset.value->size > 0) {
            // 资源视图显式传给 LvglGif（数据所有权仍属 assets mmap）。
            auto* gif = new LvglGif(static_cast<const uint8_t*>(asset.value->data), asset.value->size);
            if (gif->IsLoaded()) {
                gif->SetTelemetryAsset(emotion);
                gif->SetFrameCallback(
                    [this, gif]() { lv_image_set_src(static_cast<lv_obj_t*>(emoji_image_), gif->image_dsc()); });
                // 只有首帧真正解码成功才替换回退 glyph；此前 IsLoaded 只代表
                // GIF 头部可读，不能证明图像可显示。
                if (gif->Start()) {
                    lv_image_set_src(static_cast<lv_obj_t*>(emoji_image_), gif->image_dsc());
                    lv_obj_add_flag(static_cast<lv_obj_t*>(emoji_label_), LV_OBJ_FLAG_HIDDEN);
                    lv_obj_remove_flag(static_cast<lv_obj_t*>(emoji_image_), LV_OBJ_FLAG_HIDDEN);
                    gif_controller_ = gif;
                    using_gif = true;
                    ESP_LOGI(kTag, "SPARKBOT_GIF_STARTED asset=%.*s", static_cast<int>(emotion.size()), emotion.data());
                } else {
                    delete gif;
                    ESP_LOGW(kTag, "SPARKBOT_GIF_FIRST_FRAME_FAILED asset=%.*s", static_cast<int>(emotion.size()),
                             emotion.data());
                }
            } else {
                delete gif;
                ESP_LOGW(kTag, "SPARKBOT_GIF_LOAD_FAILED asset=%.*s", static_cast<int>(emotion.size()), emotion.data());
            }
        }
    }

    auto* emoji_label = static_cast<lv_obj_t*>(emoji_label_);
    auto* emoji_image = static_cast<lv_obj_t*>(emoji_image_);
    if (!using_gif && emotion_changed) {
        const char* utf8 = noto_emoji_get_utf8(emotion.data());
        const lv_font_t* emotion_font = &font_noto_emoji_30_4;
        if (utf8 == nullptr) {
            utf8 = material_symbols_get_utf8(emotion.data());
            emotion_font = &font_material_symbols_30_4;
        }
        if (utf8 != nullptr) {
            lv_obj_set_style_text_font(emoji_label, emotion_font, 0);
            lv_label_set_text(emoji_label, utf8);
            lv_obj_add_flag(emoji_image, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(emoji_label, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (emotion_changed) {
        current_emotion_ = std::string(emotion);
    }

    // 官方状态栏：显示快照 status_text。
    auto* status_label = static_cast<lv_obj_t*>(status_label_);
    auto* network_label = static_cast<lv_obj_t*>(network_label_);
    lv_label_set_text(network_label, snapshot.network_connected ? MATERIAL_SYMBOLS_WIFI : "");
    if (!snapshot.status_text.empty()) {
        lv_label_set_text(status_label, snapshot.status_text.c_str());
        lv_obj_remove_flag(status_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(status_label, LV_OBJ_FLAG_HIDDEN);
    }

    // 消息栏：固定一行，显式换行折叠为空格；超长内容由 LVGL label 在
    // 安全边距内做横向循环滚动，绝不把第三行挤进表情舞台。
    auto* bottom_bar = static_cast<lv_obj_t*>(bottom_bar_);
    auto* chat_message_label = static_cast<lv_obj_t*>(chat_message_label_);
    constexpr SparkBotDisplayLayout layout = DefaultSparkBotDisplayLayout();
    const lv_coord_t viewport_width = LV_HOR_RES - static_cast<lv_coord_t>(layout.horizontal_inset * 2);
    const lv_coord_t message_width = viewport_width - static_cast<lv_coord_t>(layout.content_inset * 2);
    const std::string display_text = FlattenSubtitleLine(snapshot.content_text);
    if (!snapshot.content_text.empty()) {
        lv_label_set_text(chat_message_label, display_text.c_str());
        lv_obj_set_width(chat_message_label, message_width);
        lv_obj_set_height(chat_message_label, layout.content_height);
        lv_obj_align(chat_message_label, LV_ALIGN_CENTER, 0, 0);
        lv_obj_remove_flag(bottom_bar, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text(chat_message_label, "");
        lv_obj_add_flag(bottom_bar, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_update_layout(lv_screen_active());
    lv_area_t viewport_coords{};
    lv_area_t status_coords{};
    lv_area_t content_coords{};
    lv_obj_get_coords(static_cast<lv_obj_t*>(container_), &viewport_coords);
    lv_obj_get_coords(status_label, &status_coords);
    lv_obj_get_coords(chat_message_label, &content_coords);
    const lv_font_t* content_font = lv_obj_get_style_text_font(chat_message_label, LV_PART_MAIN);
    const int32_t content_letter_space = lv_obj_get_style_text_letter_space(chat_message_label, LV_PART_MAIN);
    const int32_t content_line_space = lv_obj_get_style_text_line_space(chat_message_label, LV_PART_MAIN);
    lv_point_t content_text_size{};
    lv_text_get_size(&content_text_size, display_text.c_str(), content_font, content_letter_space, content_line_space,
                     LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    const lv_coord_t content_overflow_width =
        content_text_size.x > message_width ? content_text_size.x - message_width : 0;
    ESP_LOGI(
        kTag,
        "SPARKBOT_TEXT_RENDER generation=%llu revision=%llu status_bytes=%u content_bytes=%u "
        "viewport_xywh=%d,%d,%d,%d status_visible=%d status_xywh=%d,%d,%d,%d content_visible=%d "
        "content_xywh=%d,%d,%d,%d viewport_height=%d content_width=%d content_height=%d overflow_width=%d "
        "manual_line_breaks=0 scroll_mode=horizontal scroll_duration_ms=%u common_font=%d status=%.*s content=%.*s",
        static_cast<unsigned long long>(snapshot.generation), static_cast<unsigned long long>(snapshot.revision),
        static_cast<unsigned>(snapshot.status_text.size()), static_cast<unsigned>(snapshot.content_text.size()),
        static_cast<int>(viewport_coords.x1), static_cast<int>(viewport_coords.y1),
        static_cast<int>(lv_area_get_width(&viewport_coords)), static_cast<int>(lv_area_get_height(&viewport_coords)),
        !snapshot.status_text.empty(), static_cast<int>(status_coords.x1), static_cast<int>(status_coords.y1),
        static_cast<int>(lv_area_get_width(&status_coords)), static_cast<int>(lv_area_get_height(&status_coords)),
        !snapshot.content_text.empty(), static_cast<int>(content_coords.x1), static_cast<int>(content_coords.y1),
        static_cast<int>(lv_area_get_width(&content_coords)), static_cast<int>(lv_area_get_height(&content_coords)),
        static_cast<int>(lv_area_get_height(&viewport_coords)), static_cast<int>(content_text_size.x),
        static_cast<int>(lv_area_get_height(&content_coords)), static_cast<int>(content_overflow_width),
        static_cast<unsigned>(layout.content_scroll_duration_ms), common_text_font_ != nullptr,
        static_cast<int>(snapshot.status_text.size()), snapshot.status_text.c_str(),
        static_cast<int>(snapshot.content_text.size()), snapshot.content_text.c_str());
    return voicelife::Status::Ok();
#else
    (void)snapshot;
    return voicelife::Status::Error(voicelife::ErrorCode::kUnavailable, "主机构建不渲染真实 LVGL UI");
#endif
}

#ifdef ESP_PLATFORM
void SparkBotLvglRenderer::SetNormalUiVisible(bool visible) {
    const auto set_visibility = [visible](void* handle) {
        if (handle == nullptr) return;
        auto* object = static_cast<lv_obj_t*>(handle);
        if (visible) {
            lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
        }
    };
    set_visibility(top_bar_);
    set_visibility(status_label_);
    set_visibility(emoji_box_);
    set_visibility(bottom_bar_);
}

void SparkBotLvglRenderer::StopIdleScreenSaverGif() {
    if (screen_saver_gif_controller_ == nullptr) return;
    auto* gif = static_cast<LvglGif*>(screen_saver_gif_controller_);
    gif->Stop();
    delete gif;
    screen_saver_gif_controller_ = nullptr;
}

bool SparkBotLvglRenderer::StartIdleScreenSaverGif() {
    if (emoji_assets_ == nullptr || !assets_ready_ || screen_saver_image_ == nullptr) {
        return false;
    }
    const auto asset = emoji_assets_->Load("idle_eyes");
    if (!asset.ok() || !asset.value.has_value() || asset.value->data == nullptr || asset.value->size == 0) {
        ESP_LOGW(kTag, "SPARKBOT_IDLE_SCREENSAVER_ASSET_FAILED=1");
        return false;
    }
    auto* gif = new LvglGif(static_cast<const uint8_t*>(asset.value->data), asset.value->size);
    if (!gif->IsLoaded()) {
        delete gif;
        ESP_LOGW(kTag, "SPARKBOT_IDLE_SCREENSAVER_GIF_OPEN_FAILED=1");
        return false;
    }
    gif->SetTelemetryAsset("idle_eyes");
    gif->SetFrameCallback(
        [this, gif]() { lv_image_set_src(static_cast<lv_obj_t*>(screen_saver_image_), gif->image_dsc()); });
    if (!gif->Start()) {
        delete gif;
        ESP_LOGW(kTag, "SPARKBOT_IDLE_SCREENSAVER_FIRST_FRAME_FAILED=1");
        return false;
    }
    lv_image_set_src(static_cast<lv_obj_t*>(screen_saver_image_), gif->image_dsc());
    screen_saver_gif_controller_ = gif;
    return true;
}

void SparkBotLvglRenderer::EnterIdleScreenSaver() {
    if (screen_saver_active_ || !screen_saver_eligible_) return;
    if (!StartIdleScreenSaverGif()) {
        standby_idle_tracking_ = false;
        return;
    }
    if (gif_controller_ != nullptr) {
        auto* gif = static_cast<LvglGif*>(gif_controller_);
        gif->Stop();
        delete gif;
        gif_controller_ = nullptr;
    }
    current_emotion_.clear();
    SetNormalUiVisible(false);
    lv_obj_remove_flag(static_cast<lv_obj_t*>(screen_saver_image_), LV_OBJ_FLAG_HIDDEN);
    screen_saver_active_ = true;
    standby_idle_tracking_ = false;
    ESP_LOGI(kTag, "SPARKBOT_IDLE_SCREENSAVER_ENTERED=1 asset=idle_eyes viewport_y=%u size=%u",
             static_cast<unsigned>(DefaultSparkBotDisplayLayout().viewport_y),
             static_cast<unsigned>(DefaultSparkBotDisplayLayout().screen_saver_size));
}

void SparkBotLvglRenderer::ExitIdleScreenSaver() {
    if (!screen_saver_active_) return;
    StopIdleScreenSaverGif();
    lv_obj_add_flag(static_cast<lv_obj_t*>(screen_saver_image_), LV_OBJ_FLAG_HIDDEN);
    SetNormalUiVisible(true);
    current_emotion_.clear();
    screen_saver_active_ = false;
    standby_idle_tracking_ = false;
    ESP_LOGI(kTag, "SPARKBOT_IDLE_SCREENSAVER_EXITED=1");
}

void SparkBotLvglRenderer::ScreenSaverTimerTick() {
    if (screen_saver_active_ || !standby_idle_tracking_ || !screen_saver_eligible_) return;
    const auto layout = DefaultSparkBotDisplayLayout();
    const uint32_t elapsed = lv_tick_elaps(standby_idle_started_ms_);
    voicelife::voice::DisplaySnapshot standby_snapshot;
    standby_snapshot.phase = voicelife::voice::VoiceInteractionState::kStandby;
    standby_snapshot.role = voicelife::voice::VoiceContentRole::kNone;
    if (ShouldEnterIdleScreenSaver(standby_snapshot, elapsed, layout.screen_saver_idle_timeout_ms)) {
        EnterIdleScreenSaver();
    }
}
#endif

}  // namespace voicelife::display_sparkbot
