#include "voicelife/display_sparkbot/sparkbot_lvgl_display.h"

#include <vector>

#ifdef ESP_PLATFORM
#include <driver/gpio.h>
#include <driver/spi_common.h>
#include <esp_err.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lvgl_port.h>
#include <esp_psram.h>
#include <lvgl.h>
#include <sdkconfig.h>
#include <src/misc/cache/lv_cache.h>
#endif

namespace voicelife::display_sparkbot {

namespace {
#ifdef ESP_PLATFORM
constexpr const char* kTag = "sparkbot_lvgl";

/** @brief esp_err_t 转 Status。 */
voicelife::Status EspStatus(esp_err_t err, const char* what) {
    if (err == ESP_OK) {
        return voicelife::Status::Ok();
    }
    return voicelife::Status::Error(voicelife::ErrorCode::kInternal, std::string(what) + " 失败");
}
#endif
}  // namespace

SparkBotLvglDisplay::SparkBotLvglDisplay(const SparkBotLcdConfig& config) : config_(config) {}

SparkBotLvglDisplay::~SparkBotLvglDisplay() = default;

voicelife::Status SparkBotLvglDisplay::Initialize() {
#ifdef ESP_PLATFORM
    // 以下初始化直接移植自小智官方 SparkBot 实现（xiaozhi-esp32@37d1aee）：
    // esp_sparkbot_board.cc 的 InitializeSpi/InitializeDisplay 与
    // lcd_display.cc 的 SpiLcdDisplay 构造（SPI mode 2、40MHz、RGB565、
    // 单缓冲 width*20、buff_dma、swap_bytes）。不得自行修改布局与时序参数。

    // 官方 InitializeSpi：SPI3_HOST，MOSI/SCLK/CS/DC 由板级 Profile 提供。
    spi_bus_config_t buscfg = {};
    buscfg.mosi_io_num = config_.mosi_gpio;
    buscfg.miso_io_num = GPIO_NUM_NC;
    buscfg.sclk_io_num = config_.clk_gpio;
    buscfg.quadwp_io_num = GPIO_NUM_NC;
    buscfg.quadhd_io_num = GPIO_NUM_NC;
    buscfg.max_transfer_sz = config_.width * config_.height * sizeof(uint16_t);
    auto err = spi_bus_initialize(static_cast<spi_host_device_t>(config_.spi_host), &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        return EspStatus(err, "spi_bus_initialize");
    }

    // 官方 InitializeDisplay：panel IO（SPI mode 2，SparkBot mode 0 无图像）。
    esp_lcd_panel_io_handle_t panel_io = nullptr;
    esp_lcd_panel_io_spi_config_t io_config = {};
    io_config.cs_gpio_num = static_cast<gpio_num_t>(config_.cs_gpio);
    io_config.dc_gpio_num = static_cast<gpio_num_t>(config_.dc_gpio);
    io_config.spi_mode = config_.spi_mode;
    io_config.pclk_hz = config_.pixel_clock_hz;
    io_config.trans_queue_depth = 10;
    io_config.lcd_cmd_bits = 8;
    io_config.lcd_param_bits = 8;
    err = esp_lcd_new_panel_io_spi(static_cast<spi_host_device_t>(config_.spi_host), &io_config, &panel_io);
    if (err != ESP_OK) {
        return EspStatus(err, "esp_lcd_new_panel_io_spi");
    }

    // 官方 ST7789 面板配置：无复位、RGB 顺序、16bpp。
    esp_lcd_panel_handle_t panel = nullptr;
    esp_lcd_panel_dev_config_t panel_config = {};
    panel_config.reset_gpio_num = config_.reset_gpio < 0 ? GPIO_NUM_NC : static_cast<gpio_num_t>(config_.reset_gpio);
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_config.bits_per_pixel = 16;
    err = esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel);
    if (err != ESP_OK) {
        return EspStatus(err, "esp_lcd_new_panel_st7789");
    }
    esp_lcd_panel_reset(panel);
    esp_lcd_panel_init(panel);
    esp_lcd_panel_invert_color(panel, true);
    esp_lcd_panel_disp_on_off(panel, true);

    // 官方 SpiLcdDisplay：白屏清屏后初始化 LVGL。
    std::vector<uint16_t> buffer(static_cast<std::size_t>(config_.width), 0xFFFF);
    for (int y = 0; y < config_.height; ++y) {
        esp_lcd_panel_draw_bitmap(panel, 0, y, config_.width, y + 1, buffer.data());
    }

    lv_init();
#if CONFIG_SPIRAM
    const std::size_t psram_size_mb = static_cast<std::size_t>(esp_psram_get_size() / 1024 / 1024);
    if (psram_size_mb >= 8) {
        lv_image_cache_resize(2 * 1024 * 1024, true);
    } else if (psram_size_mb >= 2) {
        lv_image_cache_resize(512 * 1024, true);
    }
#endif

    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 1;
#if CONFIG_SOC_CPU_CORES_NUM > 1
    port_cfg.task_affinity = 1;
#endif
    lvgl_port_init(&port_cfg);

    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = panel_io,
        .panel_handle = panel,
        .control_handle = nullptr,
        .buffer_size = static_cast<uint32_t>(config_.width * 20),
        .double_buffer = false,
        .trans_size = 0,
        .hres = static_cast<uint32_t>(config_.width),
        .vres = static_cast<uint32_t>(config_.height),
        .monochrome = false,
        .rotation =
            {
                .swap_xy = config_.swap_xy,
                .mirror_x = config_.mirror_x,
                .mirror_y = config_.mirror_y,
            },
        .rounder_cb = nullptr,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags =
            {
                .buff_dma = 1,
                .buff_spiram = 0,
                .sw_rotate = 0,
                .swap_bytes = 1,
                .full_refresh = 0,
                .direct_mode = 0,
            },
    };
    lv_display_t* display = lvgl_port_add_disp(&display_cfg);
    if (display == nullptr) {
        return voicelife::Status::Error(voicelife::ErrorCode::kInternal, "lvgl_port_add_disp 返回空");
    }
    if (config_.offset_x != 0 || config_.offset_y != 0) {
        lv_display_set_offset(display, config_.offset_x, config_.offset_y);
    }
    display_ = display;
    return voicelife::Status::Ok();
#else
    (void)0;
    return voicelife::Status::Error(voicelife::ErrorCode::kUnavailable, "主机构建不初始化真实 ST7789/LVGL 显示");
#endif
}

void* SparkBotLvglDisplay::display_handle() { return display_; }

}  // namespace voicelife::display_sparkbot
