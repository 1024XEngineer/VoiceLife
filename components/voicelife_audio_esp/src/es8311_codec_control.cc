#include "es8311_codec_control.h"

#ifdef ESP_PLATFORM
#include <driver/i2c_master.h>
#include <esp_check.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <rom/ets_sys.h>
#endif

namespace voicelife::audio_esp {

#ifdef ESP_PLATFORM
namespace {

constexpr const char* kTag = "es8311";

/** @brief 寄存器写入。 @return ESP_OK 表示写入成功。 */
esp_err_t WriteReg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t value) {
    const uint8_t payload[2] = {reg, value};
    return i2c_master_transmit(dev, payload, sizeof(payload), 100);
}

/** @brief 官方 es8311 初始化寄存器序列（xiaozhi-esp32@37d1aee es8311.c）。 */
esp_err_t WriteInitSequence(i2c_master_dev_handle_t dev) {
    // 软复位。
    ESP_RETURN_ON_ERROR(WriteReg(dev, 0x00, 0x1F), kTag, "软复位");
    esp_rom_delay_us(5000);
    ESP_RETURN_ON_ERROR(WriteReg(dev, 0x0D, 0xFA), kTag, "REG0D");
    ESP_RETURN_ON_ERROR(WriteReg(dev, 0x44, 0x08), kTag, "REG44");
    ESP_RETURN_ON_ERROR(WriteReg(dev, 0x44, 0x08), kTag, "REG44");
    ESP_RETURN_ON_ERROR(WriteReg(dev, 0x01, 0x30), kTag, "REG01");
    ESP_RETURN_ON_ERROR(WriteReg(dev, 0x02, 0x00), kTag, "REG02");
    ESP_RETURN_ON_ERROR(WriteReg(dev, 0x03, 0x10), kTag, "REG03");
    ESP_RETURN_ON_ERROR(WriteReg(dev, 0x04, 0x10), kTag, "REG04");
    ESP_RETURN_ON_ERROR(WriteReg(dev, 0x05, 0x00), kTag, "REG05");
    ESP_RETURN_ON_ERROR(WriteReg(dev, 0x16, 0x24), kTag, "REG16");
    ESP_RETURN_ON_ERROR(WriteReg(dev, 0x0B, 0x00), kTag, "REG0B");
    ESP_RETURN_ON_ERROR(WriteReg(dev, 0x0C, 0x00), kTag, "REG0C");
    ESP_RETURN_ON_ERROR(WriteReg(dev, 0x10, 0x1F), kTag, "REG10");
    ESP_RETURN_ON_ERROR(WriteReg(dev, 0x11, 0x7F), kTag, "REG11");
    ESP_RETURN_ON_ERROR(WriteReg(dev, 0x00, 0x80), kTag, "REG00 slave");
    ESP_RETURN_ON_ERROR(WriteReg(dev, 0x01, 0x3F), kTag, "REG01 use_mclk");
    ESP_RETURN_ON_ERROR(WriteReg(dev, 0x06, 0x00), kTag, "REG06 清反相");
    ESP_RETURN_ON_ERROR(WriteReg(dev, 0x13, 0x10), kTag, "REG13");
    ESP_RETURN_ON_ERROR(WriteReg(dev, 0x1B, 0x0A), kTag, "REG1B");
    ESP_RETURN_ON_ERROR(WriteReg(dev, 0x1C, 0x6A), kTag, "REG1C");
    ESP_RETURN_ON_ERROR(WriteReg(dev, 0x44, 0x08), kTag, "REG44 无 DAC 参考");
    // 16kHz / 12.288MHz MCLK 系数（官方 coeff 表行）。
    ESP_RETURN_ON_ERROR(WriteReg(dev, 0x02, 0x03), kTag, "fs coeff 02");
    ESP_RETURN_ON_ERROR(WriteReg(dev, 0x05, 0x01), kTag, "fs coeff 05");
    ESP_RETURN_ON_ERROR(WriteReg(dev, 0x03, 0x01), kTag, "fs coeff 03");
    ESP_RETURN_ON_ERROR(WriteReg(dev, 0x04, 0x01), kTag, "fs coeff 04");
    ESP_RETURN_ON_ERROR(WriteReg(dev, 0x07, 0x00), kTag, "fs coeff 07");
    ESP_RETURN_ON_ERROR(WriteReg(dev, 0x08, 0x00), kTag, "fs coeff 08");
    ESP_RETURN_ON_ERROR(WriteReg(dev, 0x06, 0xFF), kTag, "fs coeff 06");
    // I2S Philips、16bit。
    ESP_RETURN_ON_ERROR(WriteReg(dev, 0x09, 0x04), kTag, "REG09 Philips");
    ESP_RETURN_ON_ERROR(WriteReg(dev, 0x0A, 0x10), kTag, "REG0A 16bit");
    // 启动序列。
    ESP_RETURN_ON_ERROR(WriteReg(dev, 0x00, 0x80), kTag, "启动 REG00");
    ESP_RETURN_ON_ERROR(WriteReg(dev, 0x01, 0x3F), kTag, "启动 REG01");
    ESP_RETURN_ON_ERROR(WriteReg(dev, 0x17, 0xBF), kTag, "启动 REG17");
    ESP_RETURN_ON_ERROR(WriteReg(dev, 0x0E, 0x02), kTag, "启动 REG0E");
    ESP_RETURN_ON_ERROR(WriteReg(dev, 0x12, 0x00), kTag, "启动 REG12 DAC");
    ESP_RETURN_ON_ERROR(WriteReg(dev, 0x14, 0x1A), kTag, "启动 REG14");
    ESP_RETURN_ON_ERROR(WriteReg(dev, 0x0D, 0x01), kTag, "启动 REG0D");
    ESP_RETURN_ON_ERROR(WriteReg(dev, 0x15, 0x40), kTag, "启动 REG15");
    ESP_RETURN_ON_ERROR(WriteReg(dev, 0x37, 0x08), kTag, "启动 REG37");
    ESP_RETURN_ON_ERROR(WriteReg(dev, 0x45, 0x00), kTag, "启动 REG45");
    return ESP_OK;
}

}  // namespace
#endif  // ESP_PLATFORM

voicelife::Status InitializeEs8311(int i2c_port, int sda_gpio, int scl_gpio, uint8_t es8311_8bit) {
#ifdef ESP_PLATFORM
    i2c_master_bus_config_t bus_config = {};
    bus_config.i2c_port = static_cast<i2c_port_num_t>(i2c_port);
    bus_config.sda_io_num = static_cast<gpio_num_t>(sda_gpio);
    bus_config.scl_io_num = static_cast<gpio_num_t>(scl_gpio);
    bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_config.glitch_ignore_cnt = 7;
    bus_config.flags.enable_internal_pullup = 1;
    i2c_master_bus_handle_t bus = nullptr;
    esp_err_t error = i2c_new_master_bus(&bus_config, &bus);
    if (error != ESP_OK) {
        return voicelife::Status::Error(voicelife::ErrorCode::kInternal, "创建 ES8311 I2C 总线失败");
    }
    i2c_device_config_t dev_config = {};
    dev_config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_config.device_address = static_cast<uint16_t>(es8311_8bit >> 1);
    dev_config.scl_speed_hz = 100000;
    i2c_master_dev_handle_t dev = nullptr;
    error = i2c_master_bus_add_device(bus, &dev_config, &dev);
    if (error != ESP_OK) {
        return voicelife::Status::Error(voicelife::ErrorCode::kInternal, "添加 ES8311 I2C 设备失败");
    }
    error = WriteInitSequence(dev);
    i2c_master_bus_rm_device(dev);
    i2c_del_master_bus(bus);
    if (error != ESP_OK) {
        return voicelife::Status::Error(voicelife::ErrorCode::kInternal, "ES8311 初始化寄存器序列失败");
    }
    return voicelife::Status::Ok();
#else
    (void)i2c_port;
    (void)sda_gpio;
    (void)scl_gpio;
    (void)es8311_8bit;
    return voicelife::Status::Error(voicelife::ErrorCode::kUnavailable, "主机构建不初始化 ES8311 I2C");
#endif
}

}  // namespace voicelife::audio_esp
