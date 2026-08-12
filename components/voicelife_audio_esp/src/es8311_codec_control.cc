#include "es8311_codec_control.h"

#ifdef ESP_PLATFORM
#include <driver/i2c_master.h>
#include <es8311_codec.h>
#include <esp_check.h>
#include <esp_codec_dev.h>
#include <esp_codec_dev_defaults.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif

namespace voicelife::audio_esp {

#ifdef ESP_PLATFORM
namespace {

constexpr const char* kTag = "es8311";

/** @brief 保留 Codec 设备句柄（音量/静音等后续控制使用）。 */
esp_codec_dev_handle_t g_codec_dev = nullptr;

/** @brief 读回并打印关键寄存器（时钟/格式/启动证据）。 */
void LogKeyRegisters(const audio_codec_ctrl_if_t* ctrl_if) {
    static constexpr uint8_t kRegs[] = {0x00, 0x01, 0x09, 0x0A, 0x17};
    for (uint8_t reg : kRegs) {
        uint8_t value = 0;
        if (ctrl_if->read_reg(ctrl_if, reg, 1, &value, 1) == ESP_OK) {
            ESP_LOGI(kTag, "ES8311_REG_READBACK reg=0x%02X value=0x%02X", reg, value);
        } else {
            ESP_LOGW(kTag, "ES8311_REG_READBACK_FAILED reg=0x%02X", reg);
        }
    }
}

}  // namespace
#endif  // ESP_PLATFORM

voicelife::Status InitializeEs8311(const Es8311ControlConfig& config) {
#ifdef ESP_PLATFORM
    // I2C 总线（复用现有总线配置模式）。
    i2c_master_bus_config_t bus_config = {};
    bus_config.i2c_port = static_cast<i2c_port_num_t>(config.i2c_port);
    bus_config.sda_io_num = static_cast<gpio_num_t>(config.sda_gpio);
    bus_config.scl_io_num = static_cast<gpio_num_t>(config.scl_gpio);
    bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_config.glitch_ignore_cnt = 7;
    bus_config.flags.enable_internal_pullup = 1;
    i2c_master_bus_handle_t bus = nullptr;
    const esp_err_t bus_err = i2c_new_master_bus(&bus_config, &bus);
    if (bus_err != ESP_OK) {
        return voicelife::Status::Error(voicelife::ErrorCode::kInternal, "创建 ES8311 I2C 总线失败");
    }

    // I2C 控制接口（官方 audio_codec）。
    audio_codec_i2c_cfg_t i2c_cfg = {};
    i2c_cfg.port = static_cast<int>(config.i2c_port);
    i2c_cfg.addr = static_cast<uint16_t>(config.es8311_8bit >> 1);
    i2c_cfg.bus_handle = bus;
    const audio_codec_ctrl_if_t* ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (ctrl_if == nullptr) {
        i2c_del_master_bus(bus);
        return voicelife::Status::Error(voicelife::ErrorCode::kInternal, "创建 ES8311 I2C 控制接口失败");
    }

    // 软件复位（官方 ResetCodec：REG00=0x1F + 5ms）。
    uint8_t reset_value = 0x1F;
    if (ctrl_if->write_reg(ctrl_if, 0x00, 1, &reset_value, 1) != ESP_OK) {
        return voicelife::Status::Error(voicelife::ErrorCode::kInternal, "ES8311 软件复位失败");
    }
    vTaskDelay(pdMS_TO_TICKS(5));

    // I2S 数据接口（使用现有双工通道）。
    audio_codec_i2s_cfg_t i2s_cfg = {};
    i2s_cfg.port = 0;
    i2s_cfg.rx_handle = config.rx_channel;
    i2s_cfg.tx_handle = config.tx_channel;
    const audio_codec_data_if_t* data_if = audio_codec_new_i2s_data(&i2s_cfg);
    if (data_if == nullptr) {
        return voicelife::Status::Error(voicelife::ErrorCode::kInternal, "创建 ES8311 I2S 数据接口失败");
    }
    const audio_codec_gpio_if_t* gpio_if = audio_codec_new_gpio();

    // ES8311 Codec（PA 不接管，由 GPIO46 板级仲裁）。
    es8311_codec_cfg_t codec_cfg = {};
    codec_cfg.ctrl_if = ctrl_if;
    codec_cfg.gpio_if = gpio_if;
    codec_cfg.codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH;
    codec_cfg.pa_pin = -1;
    codec_cfg.use_mclk = true;
    codec_cfg.hw_gain.pa_voltage = 5.0;
    codec_cfg.hw_gain.codec_dac_voltage = 3.3;
    const audio_codec_if_t* codec_if = es8311_codec_new(&codec_cfg);
    if (codec_if == nullptr) {
        return voicelife::Status::Error(voicelife::ErrorCode::kInternal, "创建 ES8311 Codec 失败");
    }

    // 打开 IN_OUT 设备（16-bit / 1ch / 官方采样率，MCLK 由 I2S x256 提供）。
    esp_codec_dev_cfg_t dev_cfg = {};
    dev_cfg.dev_type = ESP_CODEC_DEV_TYPE_IN_OUT;
    dev_cfg.codec_if = codec_if;
    dev_cfg.data_if = data_if;
    esp_codec_dev_handle_t dev = esp_codec_dev_new(&dev_cfg);
    if (dev == nullptr) {
        return voicelife::Status::Error(voicelife::ErrorCode::kInternal, "创建 ESP Codec 设备失败");
    }
    esp_codec_dev_sample_info_t fs = {};
    fs.bits_per_sample = 16;
    fs.channel = 1;
    fs.channel_mask = 0;
    fs.sample_rate = static_cast<uint32_t>(config.sample_rate_hz);
    fs.mclk_multiple = 0;
    const esp_err_t open_err = esp_codec_dev_open(dev, &fs);
    if (open_err != ESP_OK) {
        return voicelife::Status::Error(voicelife::ErrorCode::kInternal,
                                        std::string("ES8311 打开失败: ") + esp_err_to_name(open_err));
    }
    g_codec_dev = dev;
    ESP_LOGI(kTag, "ES8311_OPEN_OK sr=%d bits=16 ch=1 mclk_multiple=0", config.sample_rate_hz);
    LogKeyRegisters(ctrl_if);
    return voicelife::Status::Ok();
#else
    (void)config;
    return voicelife::Status::Error(voicelife::ErrorCode::kUnavailable, "主机构建不初始化 ES8311 I2C");
#endif
}

}  // namespace voicelife::audio_esp
