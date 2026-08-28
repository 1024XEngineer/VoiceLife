#include "support/test_support.h"
#include "voicelife/board_esp/esp32s3_board_probe.h"
#include "voicelife/board_esp/gpio46_power_arbiter.h"
#include "voicelife/board_esp/sparkbot_profile.h"

using voicelife::ErrorCode;
using voicelife::test::Check;

int main() {
    using voicelife::board_esp::BoardCapability;
    using voicelife::board_esp::BoardProbeReport;
    using voicelife::board_esp::CapabilityStatus;
    using voicelife::board_esp::Esp32s3BoardProbe;
    using voicelife::board_esp::FindCapability;
    using voicelife::board_esp::Gpio46PowerArbiter;
    using voicelife::board_esp::SparkBotProfile;

    const auto profile = SparkBotProfile();
    Check(profile.Validate().ok(), "官方 SparkBot 板级事实应形成合法 Profile");
    Check(profile.id == "esp32s3-esp-sparkbot" && profile.sku == "esp-sparkbot" && profile.target == "esp32s3",
          "SparkBot Profile 必须保留 target、id 和 SKU");
    Check(profile.expected_flash_bytes == 16U * 1024U * 1024U && profile.expected_psram_bytes == 8U * 1024U * 1024U,
          "SparkBot Profile 必须保留 16MB Flash 和 8MB PSRAM");
    Check(profile.display.spi_host == 3 && profile.display.spi_mode == 2 &&
              profile.display.pixel_clock_hz == 40U * 1000U * 1000U && profile.display.width == 240 &&
              profile.display.height == 240 && profile.display.dc_gpio == 43 && profile.display.cs_gpio == 44 &&
              profile.display.clk_gpio == 21 && profile.display.mosi_gpio == 47,
          "ST7789 必须保留官方 SPI3、mode 2、40MHz 和 GPIO");
    Check(profile.audio.i2c_port == 0 && profile.audio.i2c_sda_gpio == 4 && profile.audio.i2c_scl_gpio == 5 &&
              profile.audio.es8311_i2c_address_7bit == 0x18 && profile.audio.mclk_gpio == 45 &&
              profile.audio.ws_gpio == 41 && profile.audio.bclk_gpio == 39 && profile.audio.din_gpio == 40 &&
              profile.audio.dout_gpio == 42,
          "ES8311 必须保留官方 I2C、地址和 I2S GPIO");
    Check(profile.camera.data_gpio == std::array<int, 8>{11, 9, 8, 10, 12, 18, 17, 16} &&
              profile.camera.xclk_gpio == 15 && profile.camera.pclk_gpio == 13 && profile.camera.vsync_gpio == 6 &&
              profile.camera.hsync_gpio == 7,
          "OV2640 必须保留官方 DVP GPIO");
    Check(profile.chassis.uart_port == 1 && profile.chassis.baud_rate == 115200 && profile.chassis.tx_gpio == 38 &&
              profile.chassis.rx_gpio == 48 && profile.boot_button_gpio == 0,
          "底盘 UART1 和 BOOT GPIO 必须保留官方配置");

    const auto* imu = FindCapability(profile, BoardCapability::kImu);
    Check(imu != nullptr && imu->status == CapabilityStatus::kVerified,
          "SparkBot BMI270 已有实板 chip ID、样本与摇晃事件证据，必须标记 verified");
    const auto* display = FindCapability(profile, BoardCapability::kDisplay);
    Check(display != nullptr && display->status == CapabilityStatus::kVerified, "官方屏幕事实必须标记 verified");
    Check(FindCapability(profile, static_cast<BoardCapability>(255)) == nullptr, "未知能力标识不能伪造能力证据");

    auto invalid_identity = profile;
    invalid_identity.id.clear();
    Check(invalid_identity.Validate().code == ErrorCode::kInvalidArgument, "缺少 Profile ID 必须拒绝");
    auto invalid_capacity = profile;
    invalid_capacity.expected_flash_bytes = 8U * 1024U * 1024U;
    Check(invalid_capacity.Validate().code == ErrorCode::kInvalidArgument, "错误的 Flash 容量必须拒绝");
    auto invalid_display = profile;
    invalid_display.display.spi_mode = 0;
    Check(invalid_display.Validate().code == ErrorCode::kInvalidArgument, "错误的 ST7789 SPI mode 必须拒绝");
    auto invalid_audio = profile;
    invalid_audio.audio.output_sample_rate_hz = 24000;
    Check(invalid_audio.Validate().code == ErrorCode::kInvalidArgument, "错误的 ES8311 输出采样率必须拒绝");
    auto invalid_chassis = profile;
    invalid_chassis.chassis.baud_rate = 9600;
    Check(invalid_chassis.Validate().code == ErrorCode::kInvalidArgument, "错误的底盘 UART 波特率必须拒绝");
    auto invalid_shared = profile;
    invalid_shared.shared_power.active_high = false;
    Check(invalid_shared.Validate().code == ErrorCode::kInvalidArgument, "错误的 GPIO46 有效电平必须拒绝");
    auto invalid_pin = profile;
    invalid_pin.display.dc_gpio = 49;
    Check(invalid_pin.Validate().code == ErrorCode::kInvalidArgument, "超范围 GPIO 必须拒绝");
    auto reused_pin = profile;
    reused_pin.display.dc_gpio = reused_pin.display.cs_gpio;
    Check(reused_pin.Validate().code == ErrorCode::kInvalidArgument, "不应共享的 GPIO 必须拒绝");
    auto invalid_shared_audio_pin = profile;
    invalid_shared_audio_pin.audio.mclk_gpio = invalid_shared_audio_pin.shared_power.gpio;
    Check(invalid_shared_audio_pin.Validate().code == ErrorCode::kInvalidArgument, "GPIO46 不能同时作为音频时钟线");
    auto invalid_camera = profile;
    invalid_camera.camera.xclk_frequency_hz = 12U * 1000U * 1000U;
    Check(invalid_camera.Validate().code == ErrorCode::kInvalidArgument, "错误的 OV2640 XCLK 必须拒绝");

    Gpio46PowerArbiter power(profile.shared_power);
    Check(power.Validate().ok() && power.idle_safe() && !power.line_enabled() && !power.state().line_level,
          "待机时 GPIO46 必须关闭");
    Check(power.SetAudioOutputEnabled(true).ok() && power.line_enabled() && power.state().line_level,
          "音频输出开启时 GPIO46 必须保持开启");
    Check(power.SetBacklightEnabled(true).ok() && power.SetAudioOutputEnabled(false).ok() && power.line_enabled(),
          "音频关闭但背光开启时 GPIO46 仍必须保持开启");
    Check(power.SetBacklightEnabled(false).ok() && power.idle_safe() && !power.state().line_level,
          "两个消费者都关闭后 GPIO46 才能回到待机安全状态");

    auto invalid_shared_profile = profile.shared_power;
    invalid_shared_profile.gpio = 45;
    Gpio46PowerArbiter invalid_arbiter(invalid_shared_profile);
    Check(invalid_arbiter.Validate().code == ErrorCode::kInvalidArgument, "共享线错绑时仲裁器必须拒绝");
    Check(invalid_arbiter.SetBacklightEnabled(true).code == ErrorCode::kInvalidArgument,
          "共享线错绑时不能更新背光请求");
    Check(invalid_arbiter.SetAudioOutputEnabled(true).code == ErrorCode::kInvalidArgument,
          "共享线错绑时不能更新音频请求");

    BoardProbeReport matching_report;
    matching_report.chip_model = "ESP32-S3";
    matching_report.flash_bytes = profile.expected_flash_bytes;
    matching_report.psram_bytes = profile.expected_psram_bytes;
    matching_report.partitions.push_back({.label = "factory", .type = 0, .subtype = 0});
    Check(matching_report.matches_profile(profile), "完整芯片、容量和分区报告必须匹配 Profile");
    auto wrong_chip = matching_report;
    wrong_chip.chip_model = "ESP32";
    Check(!wrong_chip.matches_profile(profile), "错误芯片型号不能匹配 Profile");
    auto wrong_flash = matching_report;
    wrong_flash.flash_bytes = 8U * 1024U * 1024U;
    Check(!wrong_flash.matches_profile(profile), "错误 Flash 容量不能匹配 Profile");
    auto wrong_psram = matching_report;
    wrong_psram.psram_bytes = 4U * 1024U * 1024U;
    Check(!wrong_psram.matches_profile(profile), "错误 PSRAM 容量不能匹配 Profile");
    auto no_partitions = matching_report;
    no_partitions.partitions.clear();
    Check(!no_partitions.matches_profile(profile), "缺少分区报告不能匹配 Profile");

    Esp32s3BoardProbe probe;
    const auto host_result = probe.Run(profile);
    Check(host_result.status.code == ErrorCode::kUnavailable, "主机不能伪造 ESP32-S3 身份探针已执行");
    return 0;
}
