#include "voicelife/display_sparkbot/sparkbot_lvgl_renderer.h"

#ifdef ESP_PLATFORM
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
LV_FONT_DECLARE(font_noto_sans_basic_14_1);
LV_FONT_DECLARE(font_material_symbols_14_1);
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
#endif
}  // namespace

std::string_view EmotionKeyForMood(voicelife::voice::VoiceMood mood) {
    // 官方无 sad/surprised/angry 表情，VoiceLife manifest 无 neutral.gif，
    // 按视觉语义就近映射；资源均来自受控资源清单。
    switch (mood) {
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
        case voicelife::voice::VoiceMood::kAngry:
            return "error";
        case voicelife::voice::VoiceMood::kNeutral:
        default:
            return "idle";
    }
}

SparkBotLvglRenderer::~SparkBotLvglRenderer() {
#ifdef ESP_PLATFORM
    if (gif_controller_ != nullptr) {
        auto* gif = static_cast<LvglGif*>(gif_controller_);
        gif->Stop();
        delete gif;
        gif_controller_ = nullptr;
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

    // 官方简单模式布局（lcd_display.cc SetupUI，CONFIG_USE_WECHAT_MESSAGE_STYLE=n）：
    // 黑底白字 dark 主题；中央 96x96 emoji 舞台（y=60..156）、顶部状态栏
    // 192x28（y=24）、底部消息栏 224x56。布局数值不得自行修改。
    auto* screen = lv_screen_active();
    lv_obj_set_style_text_font(screen, &font_noto_sans_basic_14_1, 0);
    lv_obj_set_style_text_color(screen, kTextColor, 0);
    lv_obj_set_style_bg_color(screen, kBackgroundColor, 0);

    // 官方简单模式的完整 240x240 dark 背景容器。
    auto* container = lv_obj_create(screen);
    lv_obj_set_size(container, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_radius(container, 0, 0);
    lv_obj_set_style_pad_all(container, 0, 0);
    lv_obj_set_style_border_width(container, 0, 0);
    lv_obj_set_style_bg_color(container, kBackgroundColor, 0);
    container_ = container;

    // 中央 emoji 舞台：固定 96x96，y=60..156。
    auto* emoji_box = lv_obj_create(screen);
    lv_obj_set_size(emoji_box, 96, 96);
    lv_obj_set_style_bg_opa(emoji_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(emoji_box, 0, 0);
    lv_obj_set_style_border_width(emoji_box, 0, 0);
    lv_obj_align(emoji_box, LV_ALIGN_TOP_MID, 0, 60);
    emoji_box_ = emoji_box;

    // 字形 fallback 标签（官方 emoji_label_，默认 robot 字形）。
    auto* emoji_label = lv_label_create(emoji_box);
    lv_obj_set_style_text_font(emoji_label, &font_material_symbols_30_4, 0);
    lv_obj_set_style_text_color(emoji_label, kTextColor, 0);
    lv_label_set_text(emoji_label, MATERIAL_SYMBOLS_ROBOT_2);
    emoji_label_ = emoji_label;

    // emoji 图片节点（后续 GIF 资源接入后使用；当前隐藏）。
    auto* emoji_image = lv_img_create(emoji_box);
    lv_obj_center(emoji_image);
    lv_obj_add_flag(emoji_image, LV_OBJ_FLAG_HIDDEN);
    emoji_image_ = emoji_image;

    // 官方 top_bar：左网络图标，右侧音量、电池和能力图标。
    auto* top_bar = lv_obj_create(screen);
    lv_obj_set_size(top_bar, LV_HOR_RES, 24);
    lv_obj_set_style_radius(top_bar, 0, 0);
    lv_obj_set_style_bg_opa(top_bar, LV_OPA_50, 0);
    lv_obj_set_style_bg_color(top_bar, kBackgroundColor, 0);
    lv_obj_set_style_border_width(top_bar, 0, 0);
    lv_obj_set_style_pad_all(top_bar, 0, 0);
    lv_obj_set_style_pad_top(top_bar, 2, 0);
    lv_obj_set_style_pad_bottom(top_bar, 2, 0);
    lv_obj_set_style_pad_left(top_bar, 4, 0);
    lv_obj_set_style_pad_right(top_bar, 4, 0);
    lv_obj_set_flex_flow(top_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top_bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(top_bar, LV_SCROLLBAR_MODE_OFF);
    lv_obj_align(top_bar, LV_ALIGN_TOP_MID, 0, 0);
    top_bar_ = top_bar;

    auto* network_label = lv_label_create(top_bar);
    lv_label_set_text(network_label, MATERIAL_SYMBOLS_WIFI);
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
    lv_label_set_text(mute_label, MATERIAL_SYMBOLS_VOLUME_UP);
    lv_obj_set_style_text_font(mute_label, &font_material_symbols_14_1, 0);
    lv_obj_set_style_text_color(mute_label, kTextColor, 0);
    mute_label_ = mute_label;

    auto* battery_label = lv_label_create(right_icons);
    lv_label_set_text(battery_label, MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_FULL);
    lv_obj_set_style_text_font(battery_label, &font_material_symbols_14_1, 0);
    lv_obj_set_style_text_color(battery_label, kTextColor, 0);
    lv_obj_set_style_margin_left(battery_label, 2, 0);
    battery_label_ = battery_label;

    auto* capability_label = lv_label_create(right_icons);
    lv_label_set_text(capability_label, MATERIAL_SYMBOLS_MIC "  " MATERIAL_SYMBOLS_PHOTO_CAMERA);
    lv_obj_set_style_text_font(capability_label, &font_material_symbols_14_1, 0);
    lv_obj_set_style_text_color(capability_label, lv_color_hex(0x6DD8E8), 0);
    lv_obj_set_style_margin_left(capability_label, 2, 0);
    capability_label_ = capability_label;

    // 状态栏：192x28 @ TOP_MID y=24，官方状态标签（居中、CLIP 滚动）。
    auto* status_bar = lv_obj_create(screen);
    lv_obj_set_size(status_bar, 192, 28);
    lv_obj_set_style_radius(status_bar, 0, 0);
    lv_obj_set_style_bg_opa(status_bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(status_bar, 0, 0);
    lv_obj_set_style_pad_all(status_bar, 0, 0);
    lv_obj_set_scrollbar_mode(status_bar, LV_SCROLLBAR_MODE_OFF);
    lv_obj_align(status_bar, LV_ALIGN_TOP_MID, 0, 24);

    auto* status_label = lv_label_create(status_bar);
    lv_obj_set_width(status_label, 192);
    lv_label_set_long_mode(status_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(status_label, kTextColor, 0);
    lv_label_set_text(status_label, "");
    lv_obj_align(status_label, LV_ALIGN_CENTER, 0, 0);
    status_label_ = status_label;

    // 底部消息栏：224x56 @ BOTTOM_MID，官方 chat_message_label_（WRAP 居中）。
    auto* bottom_bar = lv_obj_create(screen);
    lv_obj_set_size(bottom_bar, 224, 56);
    lv_obj_set_style_radius(bottom_bar, 0, 0);
    lv_obj_set_style_bg_color(bottom_bar, kBackgroundColor, 0);
    lv_obj_set_style_bg_opa(bottom_bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_color(bottom_bar, kTextColor, 0);
    lv_obj_set_style_pad_all(bottom_bar, 0, 0);
    lv_obj_set_style_border_width(bottom_bar, 0, 0);
    lv_obj_set_scrollbar_mode(bottom_bar, LV_SCROLLBAR_MODE_OFF);
    lv_obj_align(bottom_bar, LV_ALIGN_BOTTOM_MID, 0, 0);

    auto* chat_message_label = lv_label_create(bottom_bar);
    lv_label_set_text(chat_message_label, "");
    lv_obj_set_width(chat_message_label, LV_HOR_RES - 32);  // spacing(8) = 8*4
    lv_label_set_long_mode(chat_message_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(chat_message_label, 56);
    lv_obj_set_style_text_align(chat_message_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(chat_message_label, kTextColor, 0);
    lv_obj_align(chat_message_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(bottom_bar, LV_OBJ_FLAG_HIDDEN);  // 有内容才显示
    bottom_bar_ = bottom_bar;
    chat_message_label_ = chat_message_label;

    // emoji GIF 资源：官方 assets 分区格式；失败不阻塞 UI（字形 fallback）。
    emoji_assets_ = new SparkBotEmojiAssets();
    assets_ready_ = emoji_assets_->Initialize().ok();
    if (!assets_ready_) {
        ESP_LOGW(kTag, "assets 分区不可用，emoji 回退字形");
    }

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
                gif->SetFrameCallback(
                    [this, gif]() { lv_image_set_src(static_cast<lv_obj_t*>(emoji_image_), gif->image_dsc()); });
                // 官方 SetEmotion：设置初始帧并启动动画播放。
                lv_image_set_src(static_cast<lv_obj_t*>(emoji_image_), gif->image_dsc());
                gif->Start();
                lv_obj_add_flag(static_cast<lv_obj_t*>(emoji_label_), LV_OBJ_FLAG_HIDDEN);
                lv_obj_remove_flag(static_cast<lv_obj_t*>(emoji_image_), LV_OBJ_FLAG_HIDDEN);
                gif_controller_ = gif;
                using_gif = true;
                ESP_LOGI(kTag, "SPARKBOT_GIF_STARTED asset=%.*s", static_cast<int>(emotion.size()), emotion.data());
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
    if (!snapshot.status_text.empty()) {
        lv_label_set_text(status_label, snapshot.status_text.c_str());
        lv_obj_remove_flag(status_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(status_label, LV_OBJ_FLAG_HIDDEN);
    }

    // 官方消息栏：有内容才显示（WRAP 换行）。
    auto* bottom_bar = static_cast<lv_obj_t*>(bottom_bar_);
    auto* chat_message_label = static_cast<lv_obj_t*>(chat_message_label_);
    if (!snapshot.content_text.empty()) {
        lv_label_set_text(chat_message_label, snapshot.content_text.c_str());
        lv_obj_remove_flag(bottom_bar, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(bottom_bar, LV_OBJ_FLAG_HIDDEN);
    }
    return voicelife::Status::Ok();
#else
    (void)snapshot;
    return voicelife::Status::Error(voicelife::ErrorCode::kUnavailable, "主机构建不渲染真实 LVGL UI");
#endif
}

}  // namespace voicelife::display_sparkbot
