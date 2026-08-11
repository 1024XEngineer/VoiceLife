#include "voicelife/display_esp/ssd1306_status_display.h"

#ifdef ESP_PLATFORM

#include <array>
#include <cctype>
#include <cstring>
#include <mutex>

#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_ssd1306.h"
#include "esp_log.h"

namespace voicelife::display_esp {
namespace {

constexpr int kWidth = 128;
constexpr int kHeight = 32;
constexpr int kPages = kHeight / 8;
constexpr i2c_port_t kI2cPort = I2C_NUM_0;
constexpr gpio_num_t kSda = GPIO_NUM_41;
constexpr gpio_num_t kScl = GPIO_NUM_42;
constexpr uint8_t kAddress = 0x3c;

std::array<uint8_t, 5> Glyph(char value) {
    switch (value) {
        case 'A':
            return {0x7e, 0x11, 0x11, 0x7e, 0x00};
        case 'B':
            return {0x7f, 0x49, 0x49, 0x36, 0x00};
        case 'C':
            return {0x3e, 0x41, 0x41, 0x22, 0x00};
        case 'D':
            return {0x7f, 0x41, 0x41, 0x3e, 0x00};
        case 'E':
            return {0x7f, 0x49, 0x49, 0x41, 0x00};
        case 'G':
            return {0x3e, 0x41, 0x49, 0x7a, 0x00};
        case 'I':
            return {0x41, 0x7f, 0x41, 0x00, 0x00};
        case 'K':
            return {0x7f, 0x08, 0x14, 0x63, 0x00};
        case 'L':
            return {0x7f, 0x40, 0x40, 0x40, 0x00};
        case 'N':
            return {0x7f, 0x06, 0x18, 0x7f, 0x00};
        case 'O':
            return {0x3e, 0x41, 0x41, 0x3e, 0x00};
        case 'R':
            return {0x7f, 0x09, 0x19, 0x66, 0x00};
        case 'S':
            return {0x46, 0x49, 0x49, 0x31, 0x00};
        case 'T':
            return {0x01, 0x7f, 0x01, 0x01, 0x00};
        case 'W':
            return {0x7f, 0x20, 0x18, 0x20, 0x7f};
        case 'V':
            return {0x07, 0x38, 0x40, 0x38, 0x07};
        case 'Y':
            return {0x03, 0x04, 0x78, 0x04, 0x03};
        case '0':
            return {0x3e, 0x45, 0x49, 0x51, 0x3e};
        case '1':
            return {0x00, 0x42, 0x7f, 0x40, 0x00};
        case '2':
            return {0x62, 0x51, 0x49, 0x49, 0x46};
        case '3':
            return {0x22, 0x49, 0x49, 0x49, 0x36};
        case '4':
            return {0x18, 0x14, 0x12, 0x7f, 0x10};
        case '5':
            return {0x2f, 0x49, 0x49, 0x49, 0x31};
        case '6':
            return {0x3e, 0x49, 0x49, 0x49, 0x32};
        case '7':
            return {0x01, 0x71, 0x09, 0x05, 0x03};
        case '8':
            return {0x36, 0x49, 0x49, 0x49, 0x36};
        case '9':
            return {0x26, 0x49, 0x49, 0x49, 0x3e};
        case '-':
            return {0x08, 0x08, 0x08, 0x08, 0x00};
        case ':':
            return {0x00, 0x36, 0x36, 0x00, 0x00};
        default:
            return {0, 0, 0, 0, 0};
    }
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

Status DrawText(DisplayState& state, std::string_view text) {
    state.buffer.fill(0);
    int x = 0;
    int page = 0;
    for (const char raw : text) {
        if (raw == '\n' || x + 6 > kWidth) {
            x = 0;
            ++page;
            if (page >= kPages) break;
            if (raw == '\n') continue;
        }
        const auto glyph = Glyph(static_cast<char>(std::toupper(static_cast<unsigned char>(raw))));
        for (int column = 0; column < 5 && x + column < kWidth; ++column) {
            state.buffer[page * kWidth + x + column] = glyph[column];
        }
        x += 6;
    }
    const esp_err_t error = esp_lcd_panel_draw_bitmap(state.panel, 0, 0, kWidth, kHeight, state.buffer.data());
    if (error != ESP_OK) {
        return Status::Error(ErrorCode::kUnavailable, "OLED SSD1306 绘制失败");
    }
    ESP_LOGI("VoiceLifeDisplay", "DISPLAY_DRAW=1 text=%.*s", static_cast<int>(text.size()), text.data());
    return Status::Ok();
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
    esp_lcd_panel_ssd1306_config_t ssd_config = {.height = kHeight};
    panel_config.vendor_config = &ssd_config;
    if (const esp_err_t error = esp_lcd_new_panel_ssd1306(state.io, &panel_config, &state.panel); error != ESP_OK) {
        return Status::Error(ErrorCode::kUnavailable, "OLED SSD1306 驱动创建失败");
    }
    if (esp_lcd_panel_reset(state.panel) != ESP_OK || esp_lcd_panel_init(state.panel) != ESP_OK ||
        esp_lcd_panel_disp_on_off(state.panel, true) != ESP_OK) {
        return Status::Error(ErrorCode::kUnavailable, "OLED SSD1306 上电失败");
    }
    // Match the bread-compact-wifi board orientation used by 小智.
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

}  // namespace voicelife::display_esp

#else

namespace voicelife::display_esp {
Status InitializeStatusDisplay() { return Status::Ok(); }
Status SetStatus(std::string_view) { return Status::Ok(); }
}  // namespace voicelife::display_esp

#endif
