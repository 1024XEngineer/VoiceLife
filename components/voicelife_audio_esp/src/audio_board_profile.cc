#include "voicelife/audio_esp/audio_board_profile.h"

#include <array>
#include <utility>

namespace voicelife::audio_esp {
namespace {

Status Invalid(std::string message) {
    return Status::Error(ErrorCode::kInvalidArgument, std::move(message));
}

bool ValidGpio(int gpio) { return gpio >= 0 && gpio <= 48; }

bool SameFormat(const voice::AudioFormat& left, const voice::AudioFormat& right) {
    return left.codec == right.codec && left.sample_rate_hz == right.sample_rate_hz &&
           left.channels == right.channels && left.bits_per_sample == right.bits_per_sample &&
           left.frame_duration_ms == right.frame_duration_ms;
}

}  // namespace

Status AudioBoardProfile::Validate() const {
    if (id.empty()) {
        return Invalid("音频 Board Profile 缺少 id");
    }
    if (i2s_port > 1) {
        return Invalid("ESP32-S3 只允许 I2S 0 或 I2S 1");
    }
    const std::array<int, 7> pins = {i2s.mclk, i2s.ws, i2s.bclk, i2s.din, i2s.dout, i2c.sda,
                                     i2c.scl};
    for (int pin : pins) {
        if (!ValidGpio(pin)) {
            return Invalid("音频 Profile 包含无效 GPIO");
        }
    }
    for (std::size_t i = 0; i < pins.size(); ++i) {
        for (std::size_t j = i + 1; j < pins.size(); ++j) {
            if (pins[i] == pins[j]) {
                return Invalid("音频 I2S 与 I2C GPIO 不能复用");
            }
        }
    }
    if (codec_addresses.es8311_8bit == 0 || (codec_addresses.es8311_8bit & 1U) != 0) {
        return Invalid("ES8311 必须使用合法的 8-bit 偶数 I2C 地址");
    }
    if (codec_addresses.es7210_8bit == 0 || (codec_addresses.es7210_8bit & 1U) != 0) {
        return Invalid("ES7210 必须使用合法的 8-bit 偶数 I2C 地址");
    }
    if (codec_addresses.pca9557_7bit == 0 || codec_addresses.pca9557_7bit >= 0x80) {
        return Invalid("PCA9557 必须使用合法的 7-bit I2C 地址");
    }
    if (!device_capture_format.valid() || !device_playback_format.valid() ||
        device_capture_format.codec != voice::AudioCodec::kPcmS16Le ||
        device_playback_format.codec != voice::AudioCodec::kPcmS16Le) {
        return Invalid("设备音频格式必须是合法的 PCM S16LE");
    }
    if (input_reference && device_capture_format.channels < 2) {
        return Invalid("启用 playback reference 时采集至少需要两个通道");
    }
    if (dma_desc_num < 2 || dma_desc_num > 16 || dma_frame_num < 64 || dma_frame_num > 1024) {
        return Invalid("I2S DMA 参数超出受支持范围");
    }
    if (SameFormat(device_capture_format, device_playback_format) &&
        device_capture_format.channels > 2) {
        return Invalid("标准 PCM I2S Probe 最多验证双声道");
    }
    return Status::Ok();
}

AudioBoardProfile LichuangEsp32s3Profile() {
    AudioBoardProfile profile;
    profile.id = "esp32s3-lichuang";
    profile.i2s_port = 0;
    profile.i2s = {.mclk = 38, .ws = 13, .bclk = 14, .din = 12, .dout = 45};
    profile.i2c = {.sda = 1, .scl = 2};
    profile.codec_addresses = {.es8311_8bit = 0x30, .es7210_8bit = 0x82, .pca9557_7bit = 0x19};
    profile.device_capture_format = {.codec = voice::AudioCodec::kPcmS16Le,
                                      .sample_rate_hz = 24000,
                                      .channels = 2,
                                      .bits_per_sample = 16,
                                      .frame_duration_ms = 10};
    profile.device_playback_format = {.codec = voice::AudioCodec::kPcmS16Le,
                                       .sample_rate_hz = 24000,
                                       .channels = 1,
                                       .bits_per_sample = 16,
                                       .frame_duration_ms = 10};
    profile.dma_desc_num = 6;
    profile.dma_frame_num = 240;
    profile.input_reference = true;
    return profile;
}

}  // namespace voicelife::audio_esp
