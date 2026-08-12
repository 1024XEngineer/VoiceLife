#include "voicelife/audio_esp/audio_board_profile.h"

#include <algorithm>
#include <array>
#include <utility>
#include <vector>

namespace voicelife::audio_esp {
namespace {

Status Invalid(std::string message) { return Status::Error(ErrorCode::kInvalidArgument, std::move(message)); }

bool ValidGpio(int gpio) { return gpio >= 0 && gpio <= 48; }

Status ValidateEndpoint(const I2sEndpointProfile& endpoint) {
    if (endpoint.port > 1) {
        return Invalid("ESP32-S3 只允许 I2S 0 或 I2S 1");
    }
    const std::array<int, 3> required_pins = {endpoint.bclk, endpoint.ws, endpoint.data};
    if (!std::all_of(required_pins.begin(), required_pins.end(), ValidGpio) ||
        (endpoint.mclk != -1 && !ValidGpio(endpoint.mclk))) {
        return Invalid("音频 Profile 包含无效 GPIO");
    }

    std::vector<int> pins(required_pins.begin(), required_pins.end());
    if (endpoint.mclk != -1) {
        pins.push_back(endpoint.mclk);
    }
    std::sort(pins.begin(), pins.end());
    if (std::adjacent_find(pins.begin(), pins.end()) != pins.end()) {
        return Invalid("单个 I2S 端点不能复用时钟与数据 GPIO");
    }
    if (!endpoint.format.valid() || endpoint.format.codec != voice::AudioCodec::kPcmS16Le ||
        endpoint.format.bits_per_sample != 16) {
        return Invalid("设备音频格式必须是合法的 PCM S16LE");
    }
    if (endpoint.wire_bits_per_sample != 16 && endpoint.wire_bits_per_sample != 32) {
        return Invalid("I2S wire sample 只支持 16-bit 或 32-bit");
    }
    if ((endpoint.wire_bits_per_sample == 16 && endpoint.pcm_shift_bits != 0) || endpoint.pcm_shift_bits > 16) {
        return Invalid("I2S PCM 对齐位数与 wire sample 不匹配");
    }
    return Status::Ok();
}

Status RejectDuplicatePins(std::vector<int> pins) {
    pins.erase(std::remove(pins.begin(), pins.end(), -1), pins.end());
    std::sort(pins.begin(), pins.end());
    if (std::adjacent_find(pins.begin(), pins.end()) != pins.end()) {
        return Invalid("音频 I2S 与 I2C GPIO 不能冲突");
    }
    return Status::Ok();
}

}  // namespace

Status AudioBoardProfile::Validate() const {
    if (id.empty()) {
        return Invalid("音频 Board Profile 缺少 id");
    }
    const Status capture_status = ValidateEndpoint(capture_i2s);
    if (!capture_status.ok()) {
        return capture_status;
    }
    const Status playback_status = ValidateEndpoint(playback_i2s);
    if (!playback_status.ok()) {
        return playback_status;
    }

    std::vector<int> pins;
    if (topology == AudioBoardTopology::kExternalCodecDuplex) {
        if (!codec_control.has_value()) {
            return Invalid("外部 Codec 双工拓扑缺少 I2C 控制配置");
        }
        if (capture_i2s.port != playback_i2s.port || capture_i2s.mclk != playback_i2s.mclk ||
            capture_i2s.bclk != playback_i2s.bclk || capture_i2s.ws != playback_i2s.ws ||
            capture_i2s.format.sample_rate_hz != playback_i2s.format.sample_rate_hz ||
            capture_i2s.wire_bits_per_sample != playback_i2s.wire_bits_per_sample) {
            return Invalid("外部 Codec 双工端点必须共享 I2S port、时钟与 wire sample");
        }
        pins = {capture_i2s.mclk, capture_i2s.bclk, capture_i2s.ws, capture_i2s.data, playback_i2s.data};
    } else {
        if (codec_control.has_value()) {
            return Invalid("纯 I2S simplex 拓扑不能携带外部 Codec 控制配置");
        }
        if (input_reference) {
            return Invalid("纯 I2S simplex Profile 不能声明未接线的 playback reference");
        }
        pins = {capture_i2s.mclk,  capture_i2s.bclk,  capture_i2s.ws,  capture_i2s.data,
                playback_i2s.mclk, playback_i2s.bclk, playback_i2s.ws, playback_i2s.data};
    }

    if (codec_control.has_value()) {
        const auto& control = *codec_control;
        if (control.i2c_port > 1 || !ValidGpio(control.i2c.sda) || !ValidGpio(control.i2c.scl) ||
            control.i2c.sda == control.i2c.scl) {
            return Invalid("外部 Codec I2C 配置无效");
        }
        if (control.addresses.es8311_8bit == 0 || (control.addresses.es8311_8bit & 1U) != 0) {
            return Invalid("ES8311 必须使用合法的 8-bit 偶数 I2C 地址");
        }
        if (control.addresses.es7210_8bit == 0 || (control.addresses.es7210_8bit & 1U) != 0) {
            return Invalid("ES7210 必须使用合法的 8-bit 偶数 I2C 地址");
        }
        if (control.addresses.pca9557_7bit == 0 || control.addresses.pca9557_7bit >= 0x80) {
            return Invalid("PCA9557 必须使用合法的 7-bit I2C 地址");
        }
        pins.push_back(control.i2c.sda);
        pins.push_back(control.i2c.scl);
    }
    const Status pin_status = RejectDuplicatePins(std::move(pins));
    if (!pin_status.ok()) {
        return pin_status;
    }
    if (input_reference && capture_i2s.format.channels < 2) {
        return Invalid("启用 playback reference 时采集至少需要两个通道");
    }
    if (dma_desc_num < 2 || dma_desc_num > 16 || dma_frame_num < 64 || dma_frame_num > 1024) {
        return Invalid("I2S DMA 参数超出受支持范围");
    }
    if (capture_i2s.format.channels > 2 || playback_i2s.format.channels > 2) {
        return Invalid("标准 PCM I2S Probe 最多验证双声道");
    }
    return Status::Ok();
}

AudioBoardProfile LichuangEsp32s3Profile() {
    AudioBoardProfile profile;
    profile.id = "esp32s3-lichuang";
    profile.topology = AudioBoardTopology::kExternalCodecDuplex;
    profile.capture_i2s.port = 0;
    profile.capture_i2s.mclk = 38;
    profile.capture_i2s.bclk = 14;
    profile.capture_i2s.ws = 13;
    profile.capture_i2s.data = 12;
    profile.capture_i2s.format = {.codec = voice::AudioCodec::kPcmS16Le,
                                  .sample_rate_hz = 24000,
                                  .channels = 2,
                                  .bits_per_sample = 16,
                                  .frame_duration_ms = 10};
    profile.playback_i2s.port = 0;
    profile.playback_i2s.mclk = 38;
    profile.playback_i2s.bclk = 14;
    profile.playback_i2s.ws = 13;
    profile.playback_i2s.data = 45;
    profile.playback_i2s.format = {.codec = voice::AudioCodec::kPcmS16Le,
                                   .sample_rate_hz = 24000,
                                   .channels = 1,
                                   .bits_per_sample = 16,
                                   .frame_duration_ms = 10};
    CodecControlProfile codec_control;
    codec_control.i2c_port = 1;
    codec_control.i2c.sda = 1;
    codec_control.i2c.scl = 2;
    codec_control.addresses.es8311_8bit = 0x30;
    codec_control.addresses.es7210_8bit = 0x82;
    codec_control.addresses.pca9557_7bit = 0x19;
    profile.codec_control = codec_control;
    profile.dma_desc_num = 6;
    profile.dma_frame_num = 240;
    profile.input_reference = true;
    return profile;
}

AudioBoardProfile VoiceLifePcbEsp32s3Profile() {
    AudioBoardProfile profile;
    profile.id = "esp32s3-voicelife-pcb-pcm";
    profile.topology = AudioBoardTopology::kDirectI2sSimplex;
    profile.capture_i2s.port = 1;
    profile.capture_i2s.bclk = 5;
    profile.capture_i2s.ws = 4;
    profile.capture_i2s.data = 6;
    profile.capture_i2s.format = {.codec = voice::AudioCodec::kPcmS16Le,
                                  .sample_rate_hz = 16000,
                                  .channels = 1,
                                  .bits_per_sample = 16,
                                  .frame_duration_ms = 10};
    profile.capture_i2s.wire_bits_per_sample = 32;
    // The old MVP used 12 here, which adds four bits of digital gain. The
    // current board probe uses 14 to retain headroom before DSP gain control.
    profile.capture_i2s.pcm_shift_bits = 14;
    profile.playback_i2s.port = 0;
    profile.playback_i2s.bclk = 15;
    profile.playback_i2s.ws = 16;
    profile.playback_i2s.data = 7;
    profile.playback_i2s.format = {.codec = voice::AudioCodec::kPcmS16Le,
                                   .sample_rate_hz = 24000,
                                   .channels = 1,
                                   .bits_per_sample = 16,
                                   .frame_duration_ms = 10};
    profile.playback_i2s.wire_bits_per_sample = 32;
    profile.playback_i2s.pcm_shift_bits = 16;
    profile.dma_desc_num = 6;
    profile.dma_frame_num = 240;
    profile.input_reference = false;
    return profile;
}

AudioBoardProfile SparkBotEsp32s3AudioProfile() {
    AudioBoardProfile profile;
    profile.id = "esp32s3-esp-sparkbot";
    profile.topology = AudioBoardTopology::kExternalCodecDuplex;
    profile.capture_i2s.port = 0;
    profile.capture_i2s.mclk = 45;
    profile.capture_i2s.bclk = 39;
    profile.capture_i2s.ws = 41;
    profile.capture_i2s.data = 40;
    profile.capture_i2s.format = {.codec = voice::AudioCodec::kPcmS16Le,
                                  .sample_rate_hz = 16000,
                                  .channels = 1,
                                  .bits_per_sample = 16,
                                  .frame_duration_ms = 20};
    profile.playback_i2s.port = 0;
    profile.playback_i2s.mclk = 45;
    profile.playback_i2s.bclk = 39;
    profile.playback_i2s.ws = 41;
    profile.playback_i2s.data = 42;
    profile.playback_i2s.format = {.codec = voice::AudioCodec::kPcmS16Le,
                                   .sample_rate_hz = 16000,
                                   .channels = 1,
                                   .bits_per_sample = 16,
                                   .frame_duration_ms = 20};
    CodecControlProfile codec_control;
    codec_control.i2c_port = 0;
    codec_control.i2c.sda = 4;
    codec_control.i2c.scl = 5;
    codec_control.addresses.es8311_8bit = 0x30;  // ES8311 7bit 0x18 含读写位
    profile.codec_control = codec_control;
    profile.dma_desc_num = 6;
    profile.dma_frame_num = 240;
    profile.input_reference = false;
    return profile;
}

}  // namespace voicelife::audio_esp
