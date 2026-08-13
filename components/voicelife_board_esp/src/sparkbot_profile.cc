#include "voicelife/board_esp/sparkbot_profile.h"

#include <algorithm>
#include <array>
#include <string>
#include <utility>
#include <vector>

namespace voicelife::board_esp {
namespace {

Status Invalid(std::string message) { return Status::Error(ErrorCode::kInvalidArgument, std::move(message)); }

bool ValidGpio(int gpio) { return gpio >= 0 && gpio <= 48; }

Status ValidateDistinctPins(const std::vector<int>& input) {
    std::vector<int> pins;
    for (const int pin : input) {
        if (pin >= 0) {
            if (!ValidGpio(pin)) {
                return Invalid("SparkBot Profile 包含无效 GPIO");
            }
            pins.push_back(pin);
        }
    }
    std::sort(pins.begin(), pins.end());
    if (std::adjacent_find(pins.begin(), pins.end()) != pins.end()) {
        return Invalid("SparkBot Profile 复用了不应共享的 GPIO");
    }
    return Status::Ok();
}

}  // namespace

Status SparkBotBoardProfile::Validate() const {
    if (id.empty() || sku.empty() || target != "esp32s3") {
        return Invalid("SparkBot Profile 缺少合法 id、sku 或 ESP32-S3 target");
    }
    if (expected_flash_bytes != 16U * 1024U * 1024U || expected_psram_bytes != 8U * 1024U * 1024U) {
        return Invalid("SparkBot Profile 的容量事实必须是 16MB Flash 和 8MB PSRAM");
    }
    if (display.spi_host != 3 || display.spi_mode != 2 || display.pixel_clock_hz != 40U * 1000U * 1000U ||
        display.width != 240 || display.height != 240 || display.offset_x != 0 || display.offset_y != 0 ||
        display.mirror_x || display.mirror_y || display.swap_xy || display.reset_gpio != -1) {
        return Invalid("SparkBot ST7789 显示参数与官方 Profile 不一致");
    }
    if (audio.i2c_port != 0 || audio.es8311_i2c_address_7bit != 0x18 || audio.input_sample_rate_hz != 16000 ||
        audio.output_sample_rate_hz != 16000) {
        return Invalid("SparkBot ES8311 音频总线参数无效");
    }
    if (chassis.uart_port != 1 || chassis.baud_rate != 115200 || boot_button_gpio != 0) {
        return Invalid("SparkBot BOOT 或底盘 UART 参数无效");
    }
    if (shared_power.gpio != 46 || !shared_power.active_high || !shared_power.backlight_shared ||
        !shared_power.audio_output_shared) {
        return Invalid("GPIO46 必须声明为高有效的功放/背光共享线");
    }

    const Status pin_status = ValidateDistinctPins({
        display.dc_gpio,     display.cs_gpio,     display.clk_gpio,    display.mosi_gpio,   audio.i2c_sda_gpio,
        audio.i2c_scl_gpio,  audio.mclk_gpio,     audio.ws_gpio,       audio.bclk_gpio,     audio.din_gpio,
        audio.dout_gpio,     camera.data_gpio[0], camera.data_gpio[1], camera.data_gpio[2], camera.data_gpio[3],
        camera.data_gpio[4], camera.data_gpio[5], camera.data_gpio[6], camera.data_gpio[7], camera.xclk_gpio,
        camera.pclk_gpio,    camera.vsync_gpio,   camera.hsync_gpio,   chassis.tx_gpio,     chassis.rx_gpio,
        boot_button_gpio,
    });
    if (!pin_status.ok()) {
        return pin_status;
    }
    if (display.backlight_gpio != shared_power.gpio || audio.mclk_gpio == shared_power.gpio ||
        audio.ws_gpio == shared_power.gpio || audio.bclk_gpio == shared_power.gpio ||
        audio.din_gpio == shared_power.gpio || audio.dout_gpio == shared_power.gpio) {
        return Invalid("GPIO46 只能作为背光/功放共享线，不能作为音频时钟或数据线");
    }
    if (camera.xclk_frequency_hz != 16U * 1000U * 1000U || camera.sccb_sda_gpio != -1 || camera.sccb_scl_gpio != -1 ||
        camera.pwdn_gpio != -1 || camera.reset_gpio != -1) {
        return Invalid("SparkBot OV2640 摄像头参数与官方 DVP 复用 I2C Profile 不一致");
    }
    return Status::Ok();
}

SparkBotBoardProfile SparkBotProfile() {
    SparkBotBoardProfile profile;
    profile.id = "esp32s3-esp-sparkbot";
    profile.sku = "esp-sparkbot";
    profile.target = "esp32s3";
    profile.expected_flash_bytes = 16U * 1024U * 1024U;
    profile.expected_psram_bytes = 8U * 1024U * 1024U;

    profile.display.dc_gpio = 43;
    profile.display.cs_gpio = 44;
    profile.display.clk_gpio = 21;
    profile.display.mosi_gpio = 47;
    profile.display.backlight_gpio = 46;

    profile.audio.i2c_sda_gpio = 4;
    profile.audio.i2c_scl_gpio = 5;
    profile.audio.es8311_i2c_address_7bit = 0x18;
    profile.audio.mclk_gpio = 45;
    profile.audio.ws_gpio = 41;
    profile.audio.bclk_gpio = 39;
    profile.audio.din_gpio = 40;
    profile.audio.dout_gpio = 42;

    profile.camera.data_gpio = {11, 9, 8, 10, 12, 18, 17, 16};
    profile.camera.xclk_gpio = 15;
    profile.camera.pclk_gpio = 13;
    profile.camera.vsync_gpio = 6;
    profile.camera.hsync_gpio = 7;

    profile.chassis.tx_gpio = 38;
    profile.chassis.rx_gpio = 48;
    profile.boot_button_gpio = 0;
    profile.shared_power.gpio = 46;
    profile.shared_power.active_high = true;
    profile.shared_power.backlight_shared = true;
    profile.shared_power.audio_output_shared = true;

    profile.capabilities = {
        CapabilityEvidence{BoardCapability::kChip, CapabilityStatus::kVerified, "ESP32-S3 rev 0.2 启动日志"},
        CapabilityEvidence{BoardCapability::kFlash, CapabilityStatus::kVerified, "esptool flash_id 报告 16MB"},
        CapabilityEvidence{BoardCapability::kPsram, CapabilityStatus::kVerified, "官方固件启动日志报告 8MB PSRAM"},
        CapabilityEvidence{BoardCapability::kDisplay, CapabilityStatus::kVerified,
                           "官方 SparkBot config.h 与 ST7789 初始化"},
        CapabilityEvidence{BoardCapability::kAudioCodec, CapabilityStatus::kVerified,
                           "官方 SparkBot ES8311 Duplex 配置与实板启动日志"},
        CapabilityEvidence{BoardCapability::kCamera, CapabilityStatus::kVerified,
                           "官方 SparkBot OV2640 DVP 配置与实板初始化证据"},
        CapabilityEvidence{BoardCapability::kChassis, CapabilityStatus::kVerified,
                           "官方 SparkBot UART1 引脚和 115200 配置"},
        CapabilityEvidence{BoardCapability::kBootButton, CapabilityStatus::kVerified, "官方 SparkBot BOOT GPIO0 配置"},
        CapabilityEvidence{BoardCapability::kSharedPower, CapabilityStatus::kVerified,
                           "官方 SparkBot GPIO46 背光/功放共用说明"},
        CapabilityEvidence{BoardCapability::kImu, CapabilityStatus::kNeedsBoardTest,
                           "未找到原理图、BOM 或实板探针证据"},
        CapabilityEvidence{BoardCapability::kTof, CapabilityStatus::kNeedsBoardTest,
                           "未找到原理图、BOM 或实板探针证据"},
        CapabilityEvidence{BoardCapability::kAmbientLight, CapabilityStatus::kNeedsBoardTest,
                           "未找到原理图、BOM 或实板探针证据"},
        CapabilityEvidence{BoardCapability::kTouch, CapabilityStatus::kNeedsBoardTest,
                           "未找到原理图、BOM 或实板探针证据"},
    };
    return profile;
}

const CapabilityEvidence* FindCapability(const SparkBotBoardProfile& profile, BoardCapability capability) {
    const auto match =
        std::find_if(profile.capabilities.begin(), profile.capabilities.end(),
                     [capability](const CapabilityEvidence& evidence) { return evidence.capability == capability; });
    return match == profile.capabilities.end() ? nullptr : &*match;
}

}  // namespace voicelife::board_esp
