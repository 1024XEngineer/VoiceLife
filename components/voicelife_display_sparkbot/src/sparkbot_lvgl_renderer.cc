#include "voicelife/display_sparkbot/sparkbot_lvgl_renderer.h"

#include <algorithm>
#include <cctype>
#include <vector>

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

// 240x240 屏幕上的 16px/25px 行高文本布局。底栏固定保留两行可读区，
// 不侵入中央表情舞台；超出的内容由 LVGL 纵向滚动完整经过视区，不能静默
// 裁掉第三行以后文本。
constexpr lv_coord_t kMessageWidth = 208;
constexpr lv_coord_t kMessageMaximumViewportHeight = 50;
constexpr uint32_t kScrollMillisecondsPerExtraLine = 1600;

std::size_t Utf8UnitBytes(std::string_view text, std::size_t offset) {
    if (offset >= text.size()) return 0;
    const uint8_t first = static_cast<uint8_t>(text[offset]);
    std::size_t width = 1;
    if ((first & 0xe0U) == 0xc0U) width = 2;
    if ((first & 0xf0U) == 0xe0U) width = 3;
    if ((first & 0xf8U) == 0xf0U) width = 4;
    return std::min(width, text.size() - offset);
}

uint32_t Utf8Codepoint(std::string_view unit) {
    if (unit.empty()) return 0;
    const uint8_t first = static_cast<uint8_t>(unit[0]);
    if (unit.size() == 1) return first;
    if (unit.size() == 2) return ((first & 0x1fU) << 6U) | (static_cast<uint8_t>(unit[1]) & 0x3fU);
    if (unit.size() == 3) {
        return ((first & 0x0fU) << 12U) | ((static_cast<uint8_t>(unit[1]) & 0x3fU) << 6U) |
               (static_cast<uint8_t>(unit[2]) & 0x3fU);
    }
    return ((first & 0x07U) << 18U) | ((static_cast<uint8_t>(unit[1]) & 0x3fU) << 12U) |
           ((static_cast<uint8_t>(unit[2]) & 0x3fU) << 6U) | (static_cast<uint8_t>(unit[3]) & 0x3fU);
}

bool IsAsciiWordCodepoint(uint32_t codepoint) {
    return codepoint < 0x80U && (std::isalnum(static_cast<unsigned char>(codepoint)) || codepoint == '_' ||
                                 codepoint == '-' || codepoint == '.');
}

bool MustNotStartLine(uint32_t codepoint) {
    switch (codepoint) {
        case ',':
        case '.':
        case '!':
        case '?':
        case ':':
        case ';':
        case '(':
        case '[':
        case '{':
        case ')':
        case ']':
        case '}':
        case 0x3001:  // 、
        case 0x3002:  // 。
        case 0x2018:  // ‘
        case 0xff01:  // ！
        case 0xff0c:  // ，
        case 0xff1a:  // ：
        case 0xff1b:  // ；
        case 0xff1f:  // ？
        case 0x2019:  // ’
        case 0x201d:  // ”
        case 0x300b:  // 》
        case 0x300a:  // 《
        case 0x300d:  // 」
        case 0x300c:  // 「
        case 0x300f:  // 』
        case 0x300e:  // 『
        case 0x3011:  // 】
        case 0x3010:  // 【
        case 0xff08:  // （
        case 0xff09:  // ）
        case 0xff3d:  // ］
        case 0xff5d:  // ｝
            return true;
        default:
            return false;
    }
}

std::string PopLastUtf8Unit(std::string* text) {
    if (text == nullptr || text->empty()) return {};
    std::size_t offset = text->size() - 1;
    while (offset > 0 && (static_cast<uint8_t>((*text)[offset]) & 0xc0U) == 0x80U) {
        --offset;
    }
    std::string unit = text->substr(offset);
    text->erase(offset);
    return unit;
}

int32_t TextWidth(std::string_view text, const lv_font_t* font) {
    lv_point_t size{};
    lv_text_get_size(&size, text.data(), font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return size.x;
}

// 仅添加显式视觉换行，保留每个原始 UTF-8 字符。实际宽度由当前 LVGL
// 字体计算，避免按字节数或“中文固定 16px”猜测而在字体 fallback 时断错。
std::string FormatSubtitleLines(std::string_view text, const lv_font_t* font, int32_t max_width,
                                uint32_t* manual_line_breaks) {
    std::string result;
    std::string line;
    uint32_t breaks = 0;
    const auto flush_line = [&]() {
        if (!result.empty()) {
            result.push_back('\n');
            ++breaks;
        }
        result.append(line);
        line.clear();
    };
    for (std::size_t offset = 0; offset < text.size();) {
        const std::size_t width = Utf8UnitBytes(text, offset);
        const std::string_view unit = text.substr(offset, width);
        offset += width;
        if (unit == "\n" || unit == "\r") {
            flush_line();
            continue;
        }
        const uint32_t codepoint = Utf8Codepoint(unit);
        std::string candidate = line;
        candidate.append(unit);
        if (line.empty() || TextWidth(candidate, font) <= max_width) {
            line = std::move(candidate);
            continue;
        }

        // 英文词与数字串整体迁移到下一行，避免把 model、2026、URL 等切断。
        if (IsAsciiWordCodepoint(codepoint) && !line.empty()) {
            std::string trailing_word;
            while (!line.empty()) {
                const std::string trailing = PopLastUtf8Unit(&line);
                if (!IsAsciiWordCodepoint(Utf8Codepoint(trailing))) {
                    line.append(trailing);
                    break;
                }
                trailing_word.insert(0, trailing);
            }
            if (!line.empty() && TextWidth(trailing_word + std::string(unit), font) <= max_width) {
                flush_line();
                line = std::move(trailing_word);
                line.append(unit);
                continue;
            }
            line.append(trailing_word);
        }

        // 中文标点或闭合括号不应成为下一行首字符。把上一字符一并迁移，
        // 以保留语义相邻关系且不超过当前行宽度。
        if (MustNotStartLine(codepoint) && !line.empty()) {
            const std::string carry = PopLastUtf8Unit(&line);
            if (!line.empty()) {
                flush_line();
                line = carry;
                line.append(unit);
                continue;
            }
            line = carry;
            line.append(unit);
            continue;
        }

        flush_line();
        line.assign(unit);
    }
    if (!line.empty() || !text.empty()) {
        flush_line();
    }
    if (manual_line_breaks != nullptr) {
        *manual_line_breaks = breaks;
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

    // 官方简单模式布局（lcd_display.cc SetupUI，CONFIG_USE_WECHAT_MESSAGE_STYLE=n）：
    // 黑底白字 dark 主题；中央 96x96 emoji 舞台（y=60..156）、顶部状态栏
    // 192x30（y=24）、底部消息栏 224x60。中央 GIF 仍维持官方 96x96。
    // common CBIN 使用同源 16px/4bpp 字体，提升状态和消息可读性；basic
    // 仅作 fallback，顶部 Material Symbols 图标不受影响。
    emoji_assets_ = new SparkBotEmojiAssets();
    assets_ready_ = emoji_assets_->Initialize().ok();
    const lv_font_t* text_font = &font_noto_sans_basic_16_4;
    if (assets_ready_) {
        const auto common_font_asset = emoji_assets_->LoadCommonTextFont();
        if (common_font_asset.ok() && common_font_asset.value.has_value()) {
            auto* common_font =
                cbin_font_create(const_cast<uint8_t*>(static_cast<const uint8_t*>(common_font_asset.value->data)));
            if (common_font != nullptr && common_font->line_height == 25 && common_font->base_line == 9 &&
                common_font->dsc != nullptr && static_cast<const lv_font_fmt_txt_dsc_t*>(common_font->dsc)->bpp == 4) {
                common_font->fallback = &font_noto_sans_basic_16_4;
                common_text_font_ = common_font;
                text_font = common_font;
                ESP_LOGI(kTag, "SPARKBOT_COMMON_FONT_READY size=16 bpp=4 line_height=25");
            } else {
                if (common_font != nullptr) {
                    cbin_font_delete(common_font);
                }
                ESP_LOGW(kTag, "common 16px 字体元数据不符合预期规格，回退 basic");
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

    // Wi-Fi 图标由 DisplaySnapshot 的受控网络语义驱动；Renderer 不读取
    // Wi-Fi 驱动或板级状态，保持 Runtime/Adapter 边界。
    auto* network_label = lv_label_create(top_bar);
    lv_label_set_text(network_label, "");
    lv_obj_set_style_text_font(network_label, &font_material_symbols_20_4, 0);
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
    lv_obj_set_style_text_font(mute_label, &font_material_symbols_20_4, 0);
    lv_obj_set_style_text_color(mute_label, kTextColor, 0);
    mute_label_ = mute_label;

    auto* battery_label = lv_label_create(right_icons);
    lv_label_set_text(battery_label, "");
    lv_obj_set_style_text_font(battery_label, &font_material_symbols_20_4, 0);
    lv_obj_set_style_text_color(battery_label, kTextColor, 0);
    lv_obj_set_style_margin_left(battery_label, 2, 0);
    battery_label_ = battery_label;

    // Material Symbols 子集不包含 ASCII space；将两个能力 glyph 拆为独立
    // label，以 margin 控制官方同等间距，避免空格落到缺字框而显示乱码。
    auto* microphone_label = lv_label_create(right_icons);
    lv_label_set_text(microphone_label, MATERIAL_SYMBOLS_MIC);
    lv_obj_set_style_text_font(microphone_label, &font_material_symbols_20_4, 0);
    lv_obj_set_style_text_color(microphone_label, lv_color_hex(0x6DD8E8), 0);
    lv_obj_set_style_margin_left(microphone_label, 2, 0);

    auto* camera_label = lv_label_create(right_icons);
    lv_label_set_text(camera_label, MATERIAL_SYMBOLS_PHOTO_CAMERA);
    lv_obj_set_style_text_font(camera_label, &font_material_symbols_20_4, 0);
    lv_obj_set_style_text_color(camera_label, lv_color_hex(0x6DD8E8), 0);
    lv_obj_set_style_margin_left(camera_label, 4, 0);
    capability_label_ = camera_label;

    // 状态栏：192x30 @ TOP_MID y=24，文字扩大后仍保留上下留白。
    auto* status_bar = lv_obj_create(screen);
    lv_obj_set_size(status_bar, 192, 30);
    lv_obj_set_style_radius(status_bar, 0, 0);
    lv_obj_set_style_bg_opa(status_bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(status_bar, 0, 0);
    lv_obj_set_style_pad_all(status_bar, 0, 0);
    // 状态栏采用绝对布局；空文本 label 固定使用 16px 字体的 25px 行框。
    lv_obj_set_style_pad_top(status_bar, 1, 0);
    lv_obj_set_style_pad_bottom(status_bar, 1, 0);
    lv_obj_set_style_layout(status_bar, LV_LAYOUT_NONE, 0);
    // CBIN 字体来自 assets mmap，不能依赖跨层对象的样式继承。状态/消息
    // label 都显式绑定同一个字体，保证常用中文在 SparkBot 上可见。
    lv_obj_set_style_text_font(status_bar, text_font, 0);
    lv_obj_set_style_text_color(status_bar, kTextColor, 0);
    lv_obj_set_style_text_opa(status_bar, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(status_bar, LV_SCROLLBAR_MODE_OFF);
    lv_obj_align(status_bar, LV_ALIGN_TOP_MID, 0, 24);

    auto* status_label = lv_label_create(status_bar);
    lv_obj_set_width(status_label, 192);
    lv_obj_set_height(status_label, text_font->line_height);
    lv_label_set_long_mode(status_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(status_label, text_font, 0);
    lv_obj_set_style_text_color(status_label, kTextColor, 0);
    lv_obj_set_style_text_opa(status_label, LV_OPA_COVER, 0);
    lv_label_set_text(status_label, "");
    lv_obj_align(status_label, LV_ALIGN_CENTER, 0, 0);
    status_label_ = status_label;

    // 底部消息栏：高度在 Render 中按换行后的实际文本高度确定。固定高度会
    // 静默裁掉第三行及之后的 STT/TTS 文本，不能把“已调用 Render”当作
    // “用户已经看见完整内容”。
    auto* bottom_bar = lv_obj_create(screen);
    lv_obj_set_width(bottom_bar, 224);
    lv_obj_set_height(bottom_bar, text_font->line_height);
    lv_obj_set_style_radius(bottom_bar, 0, 0);
    lv_obj_set_style_bg_color(bottom_bar, kBackgroundColor, 0);
    lv_obj_set_style_bg_opa(bottom_bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_font(bottom_bar, text_font, 0);
    lv_obj_set_style_text_color(bottom_bar, kTextColor, 0);
    lv_obj_set_style_text_opa(bottom_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(bottom_bar, 0, 0);
    lv_obj_set_style_border_width(bottom_bar, 0, 0);
    lv_obj_set_scrollbar_mode(bottom_bar, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(bottom_bar, LV_DIR_VER);
    lv_obj_align(bottom_bar, LV_ALIGN_BOTTOM_MID, 0, 0);

    auto* chat_message_label = lv_label_create(bottom_bar);
    lv_label_set_text(chat_message_label, "");
    lv_obj_set_width(chat_message_label, kMessageWidth);
    lv_label_set_long_mode(chat_message_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(chat_message_label, LV_SIZE_CONTENT);
    lv_obj_set_style_text_align(chat_message_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(chat_message_label, text_font, 0);
    lv_obj_set_style_text_color(chat_message_label, kTextColor, 0);
    lv_obj_set_style_text_opa(chat_message_label, LV_OPA_COVER, 0);
    lv_obj_align(chat_message_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(bottom_bar, LV_OBJ_FLAG_HIDDEN);  // 有内容才显示
    bottom_bar_ = bottom_bar;
    chat_message_label_ = chat_message_label;

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

    // 消息栏：使用标签的实际换行高度，而非字节数估计。内容超过两行时从
    // 首行平滑滚到末行；整个原文始终保留在标签中，且不遮挡中央表情。
    auto* bottom_bar = static_cast<lv_obj_t*>(bottom_bar_);
    auto* chat_message_label = static_cast<lv_obj_t*>(chat_message_label_);
    const auto* message_font = lv_obj_get_style_text_font(chat_message_label, LV_PART_MAIN);
    lv_coord_t content_height = 0;
    lv_coord_t viewport_height = 0;
    lv_coord_t overflow_height = 0;
    uint32_t scroll_duration_ms = 0;
    uint32_t manual_line_breaks = 0;
    if (!snapshot.content_text.empty()) {
        const std::string display_text =
            FormatSubtitleLines(snapshot.content_text, message_font, kMessageWidth, &manual_line_breaks);
        lv_label_set_text(chat_message_label, display_text.c_str());
        lv_obj_set_height(chat_message_label, LV_SIZE_CONTENT);
        lv_obj_align(chat_message_label, LV_ALIGN_TOP_MID, 0, 0);
        lv_obj_update_layout(chat_message_label);
        content_height = lv_obj_get_height(chat_message_label);
        viewport_height = kMessageMaximumViewportHeight;
        lv_obj_set_height(bottom_bar, viewport_height);
        lv_obj_align(bottom_bar, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_scroll_to_y(bottom_bar, 0, LV_ANIM_OFF);
        overflow_height = std::max<lv_coord_t>(content_height - viewport_height, 0);
        if (overflow_height > 0) {
            const uint32_t line_height = std::max<uint32_t>(1, message_font == nullptr ? 1 : message_font->line_height);
            const uint32_t extra_lines = (static_cast<uint32_t>(overflow_height) + line_height - 1) / line_height;
            scroll_duration_ms = extra_lines * kScrollMillisecondsPerExtraLine;
            lv_obj_set_style_anim_duration(bottom_bar, scroll_duration_ms, 0);
            lv_obj_scroll_to_y(bottom_bar, overflow_height, LV_ANIM_ON);
        } else {
            lv_obj_align(chat_message_label, LV_ALIGN_CENTER, 0, 0);
        }
        lv_obj_remove_flag(bottom_bar, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_scroll_to_y(bottom_bar, 0, LV_ANIM_OFF);
        lv_obj_add_flag(bottom_bar, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_update_layout(lv_screen_active());
    ESP_LOGI(kTag,
             "SPARKBOT_TEXT_RENDER generation=%llu revision=%llu status_bytes=%u content_bytes=%u "
             "status_visible=%d status_xywh=%d,%d,%d,%d content_visible=%d content_height=%d viewport_height=%d "
             "overflow_height=%d scroll_duration_ms=%u manual_line_breaks=%u common_font=%d status=%.*s content=%.*s",
             static_cast<unsigned long long>(snapshot.generation), static_cast<unsigned long long>(snapshot.revision),
             static_cast<unsigned>(snapshot.status_text.size()), static_cast<unsigned>(snapshot.content_text.size()),
             !snapshot.status_text.empty(), static_cast<int>(lv_obj_get_x(status_label)),
             static_cast<int>(lv_obj_get_y(status_label)), static_cast<int>(lv_obj_get_width(status_label)),
             static_cast<int>(lv_obj_get_height(status_label)), !snapshot.content_text.empty(),
             static_cast<int>(content_height), static_cast<int>(viewport_height), static_cast<int>(overflow_height),
             static_cast<unsigned>(scroll_duration_ms), static_cast<unsigned>(manual_line_breaks),
             common_text_font_ != nullptr, static_cast<int>(snapshot.status_text.size()), snapshot.status_text.c_str(),
             static_cast<int>(snapshot.content_text.size()), snapshot.content_text.c_str());
    return voicelife::Status::Ok();
#else
    (void)snapshot;
    return voicelife::Status::Error(voicelife::ErrorCode::kUnavailable, "主机构建不渲染真实 LVGL UI");
#endif
}

}  // namespace voicelife::display_sparkbot
