#include "voicelife/audio_esp/audio_board_profile.h"
#include "voicelife/audio_esp/esp32s3_audio_probe.h"

#include "support/test_support.h"

using voicelife::ErrorCode;
using voicelife::test::Check;

int main() {
    using voicelife::audio_esp::AudioBoardProfile;
    using voicelife::audio_esp::Esp32s3AudioProbe;
    using voicelife::audio_esp::LichuangEsp32s3Profile;

    const AudioBoardProfile profile = LichuangEsp32s3Profile();
    Check(profile.Validate().ok(), "旧 MVP 立创板事实应形成合法 Profile");
    Check(profile.i2s.mclk == 38 && profile.i2s.ws == 13 && profile.i2s.bclk == 14 &&
              profile.i2s.din == 12 && profile.i2s.dout == 45,
          "Profile 必须保留旧 MVP 的 I2S 引脚");
    Check(profile.codec_addresses.es8311_8bit == 0x30 &&
              profile.codec_addresses.es7210_8bit == 0x82 &&
              profile.codec_addresses.pca9557_7bit == 0x19,
          "Codec 地址必须区分 esp_codec_dev 的 8-bit 与 I2C master 的 7-bit 语义");
    Check(profile.device_capture_format.sample_rate_hz == 24000 &&
              profile.device_capture_format.channels == 2 &&
              profile.device_playback_format.channels == 1,
          "设备采集与播放格式必须独立保留参考通道");

    auto duplicate_pin = profile;
    duplicate_pin.i2c.sda = duplicate_pin.i2s.din;
    Check(duplicate_pin.Validate().code == ErrorCode::kInvalidArgument,
          "I2S 与 I2C 复用 GPIO 必须拒绝");

    auto odd_codec_address = profile;
    odd_codec_address.codec_addresses.es7210_8bit = 0x83;
    Check(odd_codec_address.Validate().code == ErrorCode::kInvalidArgument,
          "Codec 8-bit 奇数地址必须拒绝");

    auto missing_reference_channel = profile;
    missing_reference_channel.device_capture_format.channels = 1;
    Check(missing_reference_channel.Validate().code == ErrorCode::kInvalidArgument,
          "启用参考输入时单通道采集必须拒绝");

    auto oversized_dma = profile;
    oversized_dma.dma_frame_num = 2048;
    Check(oversized_dma.Validate().code == ErrorCode::kInvalidArgument,
          "超出预算的 DMA 帧数必须拒绝");

    Esp32s3AudioProbe probe;
    const auto host_result = probe.Run(profile);
    Check(host_result.status.code == ErrorCode::kUnavailable,
          "主机不能伪装成 ESP32-S3 音频探针已执行");
    return 0;
}
