#include "voicelife/display_esp/ssd1306_status_display.h"

#include "ssd1306_glyphs.h"

#ifdef ESP_PLATFORM

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <mutex>
#include <string_view>

#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_ssd1306.h"
#include "esp_log.h"

namespace voicelife::display_esp {
namespace {

constexpr int kI2cPort = 0;
constexpr gpio_num_t kSda = GPIO_NUM_41;
constexpr gpio_num_t kScl = GPIO_NUM_42;
constexpr uint8_t kAddress = 0x3c;

constexpr int kWidth = 128;
constexpr int kHeight = 32;
constexpr int kPages = kHeight / 8;

// 解码 UTF-8 首个字符，返回码点与字节宽度；非法字节按 1 字节返回 0。
uint32_t DecodeUtf8(std::string_view text, size_t& width) {
    const uint8_t b0 = static_cast<uint8_t>(text[0]);
    if (b0 < 0x80) {
        width = 1;
        return b0;
    }
    if ((b0 & 0xe0) == 0xc0 && text.size() >= 2) {
        width = 2;
        return ((b0 & 0x1f) << 6) | (static_cast<uint8_t>(text[1]) & 0x3f);
    }
    if ((b0 & 0xf0) == 0xe0 && text.size() >= 3) {
        width = 3;
        const uint32_t cp = ((b0 & 0x0f) << 12) | ((static_cast<uint8_t>(text[1]) & 0x3f) << 6) |
                            (static_cast<uint8_t>(text[2]) & 0x3f);
        // 全角标点（U+FF00-FF5E）映射到 ASCII 等价，避免多字节标点在 ASCII 分支乱码。
        if (cp >= 0xff01 && cp <= 0xff5e) {
            return cp - 0xff01 + 0x21;
        }
        return cp;
    }
    if ((b0 & 0xf8) == 0xf0 && text.size() >= 4) {
        width = 4;
        return ((b0 & 0x07) << 18) | ((static_cast<uint8_t>(text[1]) & 0x3f) << 12) |
               ((static_cast<uint8_t>(text[2]) & 0x3f) << 6) | (static_cast<uint8_t>(text[3]) & 0x3f);
    }
    width = 1;
    return 0;
}

struct DisplayState {
    i2c_master_bus_handle_t bus = nullptr;
    esp_lcd_panel_io_handle_t io = nullptr;
    esp_lcd_panel_handle_t panel = nullptr;
    std::array<uint8_t, kWidth * kPages> buffer{};
    std::mutex mutex;
    bool initialized = false;
};

DisplayState& State() {
    static DisplayState state;
    return state;
}

// 字符水平前进宽度：中文 17px，ASCII/标点 9px（16px 高度统一基线）。
size_t Advance16(uint32_t cp) {
    if ((cp >= 0x4e00 && cp <= 0x9fff) || (cp >= 0x3000 && cp <= 0x303f) || (cp >= 0xff00 && cp <= 0xffef)) {
        return 17;  // CJK 及全角标点
    }
    if (cp == ' ') return 9;
    return 9;  // ASCII 统一 9px advance
}

// 绘制一个 16x16 汉字（2 页）。返回水平前进宽度。
void DrawChinese(DisplayState& state, int x, int page, const std::array<uint8_t, 32>& glyph) {
    for (int p = 0; p < 2; ++p) {
        if (page + p >= kPages) break;
        for (int column = 0; column < 16 && x + column < kWidth; ++column) {
            state.buffer[(page + p) * kWidth + x + column] |= glyph[p * 16 + column];
        }
    }
}

// 绘制一个 Unicode 码点：统一走 16px Font16Provider（中英文同高度）。
void DrawCodepoint16(DisplayState& state, int x, int page, uint32_t cp) {
    const auto glyph = internal::LookupGlyph16(cp);
    const bool blank = std::all_of(glyph.begin(), glyph.end(), [](uint8_t b) { return b == 0; });
    if (!blank) {
        DrawChinese(state, x, page, glyph);
    }
}

// 绘制一个 ASCII 字符（5x7，1 页）。返回水平前进宽度。
void DrawAscii(DisplayState& state, int x, int page, char raw) {
    const auto glyph = internal::LookupAsciiGlyph(static_cast<char>(std::toupper(static_cast<unsigned char>(raw))));
    for (int column = 0; column < 5 && x + column < kWidth; ++column) {
        state.buffer[page * kWidth + x + column] |= glyph[column];
    }
}

Status Flush(DisplayState& state, std::string_view text) {
    const esp_err_t error = esp_lcd_panel_draw_bitmap(state.panel, 0, 0, kWidth, kHeight, state.buffer.data());
    if (error != ESP_OK) {
        return Status::Error(ErrorCode::kUnavailable, "OLED SSD1306 绘制失败");
    }
    // 屏幕可能显示配网口令或其他用户内容；串口只保留渲染事实，不回显文本。
    ESP_LOGI("VoiceLifeDisplay", "DISPLAY_DRAW=1 bytes=%u", static_cast<unsigned>(text.size()));
    return Status::Ok();
}

Status DrawText(DisplayState& state, std::string_view text) {
    state.buffer.fill(0);
    // 中文 16x16 占 2 页，ASCII 5x7 占 1 页；统一从页 1 开始以获得垂直居中。
    int page = 1;
    int x = 0;
    size_t index = 0;
    while (index < text.size() && page < kPages) {
        if (text[index] == '\n') {
            ++index;
            x = 0;
            ++page;
            continue;
        }
        size_t width = 0;
        const uint32_t cp = DecodeUtf8(text.substr(index), width);
        const size_t advance = Advance16(cp);
        if (advance >= 17) {
            // 中文/全角：16px 字形，占 2 页。
            if (x + 16 > kWidth) {
                x = 0;
                page += 2;
                if (page >= kPages) break;
            }
            DrawCodepoint16(state, x, page, cp);
            x += advance;
        } else {
            // ASCII/半角：统一 16px 高度（Font16Provider），9px advance。
            if (x + 9 > kWidth) {
                x = 0;
                ++page;
                if (page >= kPages) break;
            }
            DrawCodepoint16(state, x, page, cp);
            x += advance;
        }
        index += width;
    }
    return Flush(state, text);
}

}  // namespace

Status InitializeStatusDisplay() {
    auto& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.initialized) return Status::Ok();
    i2c_master_bus_config_t bus_config = {};
    bus_config.i2c_port = kI2cPort;
    bus_config.sda_io_num = kSda;
    bus_config.scl_io_num = kScl;
    bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_config.glitch_ignore_cnt = 7;
    bus_config.flags.enable_internal_pullup = 1;
    if (const esp_err_t error = i2c_new_master_bus(&bus_config, &state.bus); error != ESP_OK) {
        return Status::Error(ErrorCode::kUnavailable, "OLED I2C 总线初始化失败");
    }
    esp_lcd_panel_io_i2c_config_t io_config = {};
    io_config.dev_addr = kAddress;
    io_config.scl_speed_hz = 400000;
    io_config.control_phase_bytes = 1;
    io_config.dc_bit_offset = 6;
    io_config.lcd_cmd_bits = 8;
    io_config.lcd_param_bits = 8;
    if (const esp_err_t error = esp_lcd_new_panel_io_i2c(state.bus, &io_config, &state.io); error != ESP_OK) {
        return Status::Error(ErrorCode::kUnavailable, "OLED SSD1306 I2C 面板初始化失败");
    }
    esp_lcd_panel_dev_config_t panel_config = {};
    panel_config.reset_gpio_num = GPIO_NUM_NC;
    panel_config.bits_per_pixel = 1;
    esp_lcd_panel_ssd1306_config_t ssd_config = {};
    ssd_config.height = kHeight;
    panel_config.vendor_config = &ssd_config;
    if (const esp_err_t error = esp_lcd_new_panel_ssd1306(state.io, &panel_config, &state.panel); error != ESP_OK) {
        return Status::Error(ErrorCode::kUnavailable, "OLED SSD1306 驱动创建失败");
    }
    if (esp_lcd_panel_reset(state.panel) != ESP_OK || esp_lcd_panel_init(state.panel) != ESP_OK ||
        esp_lcd_panel_disp_on_off(state.panel, true) != ESP_OK) {
        return Status::Error(ErrorCode::kUnavailable, "OLED SSD1306 上电失败");
    }
    // 实板面板需要双轴镜像，才能使正常装配方向下的文字正向显示。
    if (esp_lcd_panel_mirror(state.panel, true, true) != ESP_OK ||
        esp_lcd_panel_invert_color(state.panel, false) != ESP_OK) {
        return Status::Error(ErrorCode::kUnavailable, "OLED SSD1306 显示方向配置失败");
    }
    state.initialized = true;
    const Status draw_status = DrawText(state, "BOOT");
    if (!draw_status.ok()) {
        state.initialized = false;
        return draw_status;
    }
    ESP_LOGI("VoiceLifeDisplay", "DISPLAY_READY=1 bus=0 sda=41 scl=42 addr=0x3c size=128x32 mirror=1,1");
    return Status::Ok();
}

Status SetStatus(std::string_view status) {
    auto& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (!state.initialized) return Status::Error(ErrorCode::kUnavailable, "OLED 状态屏尚未初始化");
    return DrawText(state, status);
}

Status SetEmotion(std::string_view mood, std::string_view status, std::string_view content, size_t scroll_offset) {
    auto& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (!state.initialized) return Status::Error(ErrorCode::kUnavailable, "OLED 状态屏尚未初始化");
    state.buffer.fill(0);
    // 布局：左侧单个 16x16 牛头（x=2，页 1 垂直居中），右侧上行状态栏（x20 起，
    // 页 0-1）与下行内容栏（x20 起，页 2-3）。DrawChinese 一次即画完整 16x16，
    // 不得循环多次（否则上下堆叠出两个牛头）。
    const auto mascot = internal::LookupMoodGlyph(mood);
    DrawChinese(state, 2, 1, mascot);
    // 上行状态栏：页 0-1。
    {
        int x = 20;
        int page = 0;
        size_t index = 0;
        while (index < status.size() && page + 2 <= 2) {
            size_t width = 0;
            const uint32_t cp = DecodeUtf8(status.substr(index), width);
            if (cp >= 0x4e00 && cp <= 0x9fff) {
                const auto glyph = internal::LookupGlyph16(cp);
                const bool blank = std::all_of(glyph.begin(), glyph.end(), [](uint8_t b) { return b == 0; });
                if (!blank && x + 16 > kWidth) break;
                DrawChinese(state, x, page, glyph);
                x += blank ? 16 : 17;
            } else {
                if (x + 6 > kWidth) break;
                DrawAscii(state, x, page, static_cast<char>(cp));
                x += 6;
            }
            index += width;
        }
    }
    // 下行内容栏：页 2-3；超宽时由 scroll_offset 逐字符滚动显示。
    if (!content.empty()) {
        // 跳过 scroll_offset 个字符（滚动窗口起点）。
        size_t skipped = 0;
        size_t index = 0;
        while (index < content.size() && skipped < scroll_offset) {
            size_t width = 0;
            (void)DecodeUtf8(content.substr(index), width);
            if (width == 0) width = 1;
            index += width;
            ++skipped;
        }
        int x = 20;
        int page = 2;
        while (index < content.size() && page + 2 <= kPages) {
            size_t width = 0;
            const uint32_t cp = DecodeUtf8(content.substr(index), width);
            const size_t advance = Advance16(cp);
            if (advance >= 17) {
                if (x + 16 > kWidth) break;
                DrawCodepoint16(state, x, page, cp);
            } else {
                if (x + 9 > kWidth) break;
                // cp 已由 DecodeUtf8 归一化（全角标点映射为 ASCII 等价），
                // 统一 16px 高度（Font16Provider），F/P 等英文可见。
                DrawCodepoint16(state, x, page, cp);
            }
            x += advance;
            index += width;
        }
    }
    return Flush(state, content.empty() ? status : content);
}

}  // namespace voicelife::display_esp

#else

namespace voicelife::display_esp {
Status InitializeStatusDisplay() { return Status::Ok(); }
Status SetStatus(std::string_view) { return Status::Ok(); }
Status SetEmotion(std::string_view, std::string_view, std::string_view, size_t) { return Status::Ok(); }
}  // namespace voicelife::display_esp

#endif
