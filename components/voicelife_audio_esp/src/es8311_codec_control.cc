#include "es8311_codec_control.h"

#ifdef ESP_PLATFORM
#include <audio_codec_ctrl_if.h>
#include <audio_codec_data_if.h>
#include <audio_codec_gpio_if.h>
#include <audio_codec_if.h>
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

struct Es8311ControlHandle {
    esp_codec_dev_handle_t device = nullptr;
    const audio_codec_if_t* codec_if = nullptr;
    const audio_codec_ctrl_if_t* ctrl_if = nullptr;
    const audio_codec_gpio_if_t* gpio_if = nullptr;
    const audio_codec_data_if_t* data_if = nullptr;
    i2c_master_bus_handle_t i2c_bus = nullptr;
};

void ReleaseEs8311Resources(esp_codec_dev_handle_t device, const audio_codec_if_t* codec_if,
                            const audio_codec_ctrl_if_t* ctrl_if, const audio_codec_gpio_if_t* gpio_if,
                            const audio_codec_data_if_t* data_if, i2c_master_bus_handle_t i2c_bus) {
    if (device != nullptr) {
        (void)esp_codec_dev_close(device);
        esp_codec_dev_delete(device);
    }
    if (codec_if != nullptr) audio_codec_delete_codec_if(codec_if);
    if (ctrl_if != nullptr) audio_codec_delete_ctrl_if(ctrl_if);
    if (gpio_if != nullptr) audio_codec_delete_gpio_if(gpio_if);
    if (data_if != nullptr) audio_codec_delete_data_if(data_if);
    if (i2c_bus != nullptr) (void)i2c_del_master_bus(i2c_bus);
}

/** @brief 读回并打印关键寄存器（时钟/格式/启动证据）。 */
void LogKeyRegisters(const audio_codec_ctrl_if_t* ctrl_if) {
    // REG16 is the microphone PGA setting. Keeping it in the allowlisted
    // startup readback makes the board's capture gain observable without
    // logging audio content.
    static constexpr uint8_t kRegs[] = {0x00, 0x01, 0x09, 0x0A, 0x10, 0x17};
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

voicelife::Result<void*> InitializeEs8311(const Es8311ControlConfig& config) {
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
        return voicelife::Result<void*>::Failure(voicelife::ErrorCode::kInternal, "创建 ES8311 I2C 总线失败");
    }
    // 等 MCLK/上电稳定（I2S 已 enable，MCLK x256 输出）。
    vTaskDelay(pdMS_TO_TICKS(10));

    // 诊断：I2C ACK 探测（0x18 7bit / 0x30 8bit 同址）。
    const esp_err_t probe_err = i2c_master_probe(bus, static_cast<uint16_t>(config.es8311_8bit >> 1), 50);
    if (probe_err != ESP_OK) {
        ESP_LOGE(kTag, "ES8311_I2C_NAK addr=0x%02X err=%s 检查 MCLK/电源/引脚", config.es8311_8bit >> 1,
                 esp_err_to_name(probe_err));
    } else {
        ESP_LOGI(kTag, "ES8311_I2C_ACK addr=0x%02X", config.es8311_8bit >> 1);
    }

    // I2C 控制接口（官方 audio_codec）。
    audio_codec_i2c_cfg_t i2c_cfg = {};
    i2c_cfg.port = static_cast<int>(config.i2c_port);
    // audio_codec_ctrl_i2c 内部对 addr >> 1，这里必须传 8-bit 地址（0x30）。
    i2c_cfg.addr = static_cast<uint16_t>(config.es8311_8bit);
    i2c_cfg.bus_handle = bus;
    const audio_codec_ctrl_if_t* ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (ctrl_if == nullptr) {
        i2c_del_master_bus(bus);
        return voicelife::Result<void*>::Failure(voicelife::ErrorCode::kInternal, "创建 ES8311 I2C 控制接口失败");
    }

    // 软件复位（官方 ResetCodec：REG00=0x1F + 5ms）。
    uint8_t reset_value = 0x1F;
    if (ctrl_if->write_reg(ctrl_if, 0x00, 1, &reset_value, 1) != ESP_OK) {
        ReleaseEs8311Resources(nullptr, nullptr, ctrl_if, nullptr, nullptr, bus);
        return voicelife::Result<void*>::Failure(voicelife::ErrorCode::kInternal, "ES8311 软件复位失败");
    }
    vTaskDelay(pdMS_TO_TICKS(5));

    // I2S 数据接口（使用现有双工通道）。
    audio_codec_i2s_cfg_t i2s_cfg = {};
    i2s_cfg.port = 0;
    i2s_cfg.rx_handle = config.rx_channel;
    i2s_cfg.tx_handle = config.tx_channel;
    const audio_codec_data_if_t* data_if = audio_codec_new_i2s_data(&i2s_cfg);
    if (data_if == nullptr) {
        ReleaseEs8311Resources(nullptr, nullptr, ctrl_if, nullptr, nullptr, bus);
        return voicelife::Result<void*>::Failure(voicelife::ErrorCode::kInternal, "创建 ES8311 I2S 数据接口失败");
    }
    const audio_codec_gpio_if_t* gpio_if = audio_codec_new_gpio();
    if (gpio_if == nullptr) {
        ReleaseEs8311Resources(nullptr, nullptr, ctrl_if, nullptr, data_if, bus);
        return voicelife::Result<void*>::Failure(voicelife::ErrorCode::kInternal, "创建 ES8311 GPIO 接口失败");
    }

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
        ReleaseEs8311Resources(nullptr, nullptr, ctrl_if, gpio_if, data_if, bus);
        return voicelife::Result<void*>::Failure(voicelife::ErrorCode::kInternal, "创建 ES8311 Codec 失败");
    }

    // 打开 IN_OUT 设备（16-bit / 1ch / 官方采样率，MCLK 由 I2S x256 提供）。
    esp_codec_dev_cfg_t dev_cfg = {};
    dev_cfg.dev_type = ESP_CODEC_DEV_TYPE_IN_OUT;
    dev_cfg.codec_if = codec_if;
    dev_cfg.data_if = data_if;
    esp_codec_dev_handle_t dev = esp_codec_dev_new(&dev_cfg);
    if (dev == nullptr) {
        ReleaseEs8311Resources(nullptr, codec_if, ctrl_if, gpio_if, data_if, bus);
        return voicelife::Result<void*>::Failure(voicelife::ErrorCode::kInternal, "创建 ESP Codec 设备失败");
    }
    esp_codec_dev_sample_info_t fs = {};
    fs.bits_per_sample = 16;
    fs.channel = 1;
    fs.channel_mask = 0;
    fs.sample_rate = static_cast<uint32_t>(config.sample_rate_hz);
    fs.mclk_multiple = 0;
    const esp_err_t open_err = esp_codec_dev_open(dev, &fs);
    if (open_err != ESP_OK) {
        ReleaseEs8311Resources(dev, codec_if, ctrl_if, gpio_if, data_if, bus);
        return voicelife::Result<void*>::Failure(voicelife::ErrorCode::kInternal,
                                                 std::string("ES8311 打开失败: ") + esp_err_to_name(open_err));
    }
    // Mirror XiaoZhi's SparkBot ES8311 setup. The codec default is 0 dB;
    // with the enclosed SparkBot microphone this leaves WakeNet with an
    // unusably low signal level. The codec driver quantizes 30 dB to its
    // supported PGA step and writes ADC REG16.
    constexpr float kSparkBotMicGainDb = 30.0F;
    const esp_err_t gain_err = esp_codec_dev_set_in_gain(dev, kSparkBotMicGainDb);
    if (gain_err != ESP_OK) {
        ReleaseEs8311Resources(dev, codec_if, ctrl_if, gpio_if, data_if, bus);
        return voicelife::Result<void*>::Failure(
            voicelife::ErrorCode::kInternal,
            std::string("ES8311 设置 30dB 麦克风增益失败: ") + esp_err_to_name(gain_err));
    }
    // esp_codec_dev initializes output volume to 0. XiaoZhi explicitly
    // restores its user volume immediately after open; without this call the
    // ES8311 remains muted even when PCM reaches the I2S TX channel.
    // Match Runtime's output volume before the first PCM frame.
#if CONFIG_VOICELIFE_SERIAL_VOICE_TEST
    constexpr uint8_t kSparkBotDefaultOutputVolume = 35;
#else
    constexpr uint8_t kSparkBotDefaultOutputVolume = 100;
#endif
    const esp_err_t volume_err = esp_codec_dev_set_out_vol(dev, kSparkBotDefaultOutputVolume);
    if (volume_err != ESP_OK) {
        ReleaseEs8311Resources(dev, codec_if, ctrl_if, gpio_if, data_if, bus);
        return voicelife::Result<void*>::Failure(
            voicelife::ErrorCode::kInternal,
            std::string("ES8311 设置默认播放音量失败: ") + esp_err_to_name(volume_err));
    }
    ESP_LOGI(kTag, "ES8311_OPEN_OK sr=%d bits=16 ch=1 mclk_multiple=0", config.sample_rate_hz);
    ESP_LOGI(kTag, "ES8311_MIC_GAIN_SET db=%.1f", static_cast<double>(kSparkBotMicGainDb));
    ESP_LOGI(kTag, "ES8311_OUTPUT_VOLUME_SET value=%u", static_cast<unsigned>(kSparkBotDefaultOutputVolume));
    LogKeyRegisters(ctrl_if);
    auto* handle = new Es8311ControlHandle{.device = dev,
                                           .codec_if = codec_if,
                                           .ctrl_if = ctrl_if,
                                           .gpio_if = gpio_if,
                                           .data_if = data_if,
                                           .i2c_bus = bus};
    return voicelife::Result<void*>::Success(handle);
#else
    (void)config;
    return voicelife::Result<void*>::Failure(voicelife::ErrorCode::kUnavailable, "主机构建不初始化 ES8311 I2C");
#endif
}

voicelife::Status ReadEs8311Pcm(void* dev_handle, int16_t* samples, std::size_t sample_count) {
#ifdef ESP_PLATFORM
    if (dev_handle == nullptr || samples == nullptr || sample_count == 0) {
        return voicelife::Status::Error(voicelife::ErrorCode::kInvalidArgument, "ES8311 读取参数无效");
    }
    auto* handle = static_cast<Es8311ControlHandle*>(dev_handle);
    const esp_err_t result = esp_codec_dev_read(handle->device, samples, sample_count * sizeof(int16_t));
    if (result != ESP_OK) {
        return voicelife::Status::Error(voicelife::ErrorCode::kUnavailable,
                                        std::string("ES8311 PCM 读取失败: ") + esp_err_to_name(result));
    }
    return voicelife::Status::Ok();
#else
    (void)dev_handle;
    (void)samples;
    (void)sample_count;
    return voicelife::Status::Error(voicelife::ErrorCode::kUnavailable, "主机构建不读取 ES8311 PCM");
#endif
}

voicelife::Status WriteEs8311Pcm(void* dev_handle, const int16_t* samples, std::size_t sample_count) {
#ifdef ESP_PLATFORM
    if (dev_handle == nullptr || samples == nullptr || sample_count == 0) {
        return voicelife::Status::Error(voicelife::ErrorCode::kInvalidArgument, "ES8311 写入参数无效");
    }
    auto* handle = static_cast<Es8311ControlHandle*>(dev_handle);
    // The upstream esp_codec_dev API takes void* although it only consumes the
    // buffer synchronously. Keep the VoiceLife boundary read-only.
    const esp_err_t result =
        esp_codec_dev_write(handle->device, const_cast<int16_t*>(samples), sample_count * sizeof(int16_t));
    if (result != ESP_OK) {
        return voicelife::Status::Error(voicelife::ErrorCode::kUnavailable,
                                        std::string("ES8311 PCM 写入失败: ") + esp_err_to_name(result));
    }
    return voicelife::Status::Ok();
#else
    (void)dev_handle;
    (void)samples;
    (void)sample_count;
    return voicelife::Status::Error(voicelife::ErrorCode::kUnavailable, "主机构建不写入 ES8311 PCM");
#endif
}

voicelife::Status SetEs8311OutputVolume(void* dev_handle, uint8_t volume) {
#ifdef ESP_PLATFORM
    if (dev_handle == nullptr) {
        return voicelife::Status::Error(voicelife::ErrorCode::kInvalidArgument, "ES8311 音量句柄无效");
    }
    auto* handle = static_cast<Es8311ControlHandle*>(dev_handle);
    const esp_err_t result = esp_codec_dev_set_out_vol(handle->device, volume);
    if (result != ESP_OK) {
        return voicelife::Status::Error(voicelife::ErrorCode::kUnavailable,
                                        std::string("ES8311 设置播放音量失败: ") + esp_err_to_name(result));
    }
    return voicelife::Status::Ok();
#else
    (void)dev_handle;
    (void)volume;
    return voicelife::Status::Error(voicelife::ErrorCode::kUnavailable, "主机构建不设置 ES8311 音量");
#endif
}

voicelife::Status DeinitializeEs8311(void* dev_handle) {
#ifdef ESP_PLATFORM
    if (dev_handle == nullptr) {
        return voicelife::Status::Ok();
    }
    auto* handle = static_cast<Es8311ControlHandle*>(dev_handle);
    ReleaseEs8311Resources(handle->device, handle->codec_if, handle->ctrl_if, handle->gpio_if, handle->data_if,
                           handle->i2c_bus);
    delete handle;
    return voicelife::Status::Ok();
#else
    (void)dev_handle;
    return voicelife::Status::Ok();
#endif
}

}  // namespace voicelife::audio_esp
