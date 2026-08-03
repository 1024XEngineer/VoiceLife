#include "voicelife/audio_esp/audio_board_profile.h"
#include "voicelife/audio_esp/esp32s3_audio_probe.h"

#include "support/test_support.h"

using voicelife::ErrorCode;
using voicelife::test::Check;

int main() {
    using voicelife::audio_esp::AudioBoardTopology;
    using voicelife::audio_esp::AudioBoardProfile;
    using voicelife::audio_esp::AudioProbeReport;
    using voicelife::audio_esp::Esp32s3AudioProbe;
    using voicelife::audio_esp::LichuangEsp32s3Profile;
    using voicelife::audio_esp::VoiceLifePcbEsp32s3Profile;

    const AudioBoardProfile profile = LichuangEsp32s3Profile();
    Check(profile.Validate().ok(), "旧 MVP 立创板事实应形成合法 Profile");
    Check(profile.topology == AudioBoardTopology::kExternalCodecDuplex,
          "立创板必须声明外部 Codec 双工拓扑");
    Check(profile.capture_i2s.mclk == 38 && profile.capture_i2s.ws == 13 &&
              profile.capture_i2s.bclk == 14 && profile.capture_i2s.data == 12 &&
              profile.playback_i2s.data == 45,
          "Profile 必须保留旧 MVP 的 I2S 引脚");
    Check(profile.codec_control.has_value() &&
              profile.codec_control->addresses.es8311_8bit == 0x30 &&
              profile.codec_control->addresses.es7210_8bit == 0x82 &&
              profile.codec_control->addresses.pca9557_7bit == 0x19,
          "Codec 地址必须区分 esp_codec_dev 的 8-bit 与 I2C master 的 7-bit 语义");
    Check(profile.capture_i2s.format.sample_rate_hz == 24000 &&
              profile.capture_i2s.format.channels == 2 &&
              profile.playback_i2s.format.channels == 1,
          "设备采集与播放格式必须独立保留参考通道");

    auto duplicate_pin = profile;
    duplicate_pin.codec_control->i2c.sda = duplicate_pin.capture_i2s.data;
    Check(duplicate_pin.Validate().code == ErrorCode::kInvalidArgument,
          "I2S 与 I2C 复用 GPIO 必须拒绝");

    auto odd_codec_address = profile;
    odd_codec_address.codec_control->addresses.es7210_8bit = 0x83;
    Check(odd_codec_address.Validate().code == ErrorCode::kInvalidArgument,
          "Codec 8-bit 奇数地址必须拒绝");

    auto missing_reference_channel = profile;
    missing_reference_channel.capture_i2s.format.channels = 1;
    Check(missing_reference_channel.Validate().code == ErrorCode::kInvalidArgument,
          "启用参考输入时单通道采集必须拒绝");

    auto codec_without_control = profile;
    codec_without_control.codec_control.reset();
    Check(codec_without_control.Validate().code == ErrorCode::kInvalidArgument,
          "外部 Codec 拓扑缺少 I2C 控制能力时必须拒绝");

    auto codec_with_split_clocks = profile;
    codec_with_split_clocks.capture_i2s.ws = 4;
    Check(codec_with_split_clocks.Validate().code == ErrorCode::kInvalidArgument,
          "双工 Codec 的采集与播放时钟不一致时必须拒绝");

    auto oversized_dma = profile;
    oversized_dma.dma_frame_num = 2048;
    Check(oversized_dma.Validate().code == ErrorCode::kInvalidArgument,
          "超出预算的 DMA 帧数必须拒绝");

    const AudioBoardProfile voicelife_pcb = VoiceLifePcbEsp32s3Profile();
    Check(voicelife_pcb.Validate().ok(), "当前 VoiceLife PCB 应形成合法的纯 I2S Profile");
    Check(voicelife_pcb.id == "esp32s3-voicelife-pcb-pcm" &&
              voicelife_pcb.topology == AudioBoardTopology::kDirectI2sSimplex,
          "当前板必须使用独立的纯 I2S simplex 身份");
    Check(voicelife_pcb.capture_i2s.port == 1 && voicelife_pcb.capture_i2s.bclk == 5 &&
              voicelife_pcb.capture_i2s.ws == 4 && voicelife_pcb.capture_i2s.data == 6 &&
              voicelife_pcb.capture_i2s.format.sample_rate_hz == 16000,
          "当前板麦克风必须保留 I2S1 与 16 kHz 物理事实");
    Check(voicelife_pcb.playback_i2s.port == 0 && voicelife_pcb.playback_i2s.bclk == 15 &&
              voicelife_pcb.playback_i2s.ws == 16 && voicelife_pcb.playback_i2s.data == 7 &&
              voicelife_pcb.playback_i2s.format.sample_rate_hz == 24000,
          "当前板扬声器必须保留 I2S0 与 24 kHz 物理事实");
    Check(!voicelife_pcb.codec_control.has_value() &&
              voicelife_pcb.capture_i2s.wire_bits_per_sample == 32 &&
              voicelife_pcb.capture_i2s.pcm_shift_bits == 14 &&
              voicelife_pcb.playback_i2s.pcm_shift_bits == 16,
          "纯 I2S Profile 不得伪造 Codec，并须表达 32-bit slot 的 PCM 对齐");

    auto simplex_with_codec = voicelife_pcb;
    simplex_with_codec.codec_control = profile.codec_control;
    Check(simplex_with_codec.Validate().code == ErrorCode::kInvalidArgument,
          "纯 I2S 拓扑携带 Codec 控制字段时必须拒绝");

    auto simplex_same_port = voicelife_pcb;
    simplex_same_port.capture_i2s.port = simplex_same_port.playback_i2s.port;
    Check(simplex_same_port.Validate().ok(),
          "ESP32-S3 独立 simplex TX/RX 可以复用同一 I2S controller");

    auto simplex_duplicate_pin = voicelife_pcb;
    simplex_duplicate_pin.capture_i2s.data = simplex_duplicate_pin.playback_i2s.data;
    Check(simplex_duplicate_pin.Validate().code == ErrorCode::kInvalidArgument,
          "独立 simplex 端点的 GPIO 冲突必须拒绝");

    AudioProbeReport direct_report;
    direct_report.codec_control_required = false;
    direct_report.i2s_channels_ready = true;
    direct_report.i2s_channels_started = true;
    Check(direct_report.hardware_ready(), "纯 I2S 硬件就绪不应依赖不存在的 Codec ACK");

    direct_report.capture_samples = 320;
    direct_report.nonzero_samples = 300;
    direct_report.changed_samples = 240;
    direct_report.saturated_samples = 16;
    direct_report.peak_abs = 512;
    direct_report.sum_squares = 320U * 4096U;
    Check(direct_report.capture_signal_detected(), "具有峰值、能量和变化的 PCM 应通过信号判定");
    Check(direct_report.saturation_ratio_ppm() == 50000,
          "PCM 报告应提供不依赖浮点数的削波比例");

    direct_report.changed_samples = 0;
    Check(!direct_report.capture_signal_detected(), "固定直流值不能冒充真实麦克风信号");

    AudioProbeReport codec_report;
    codec_report.codec_control_required = true;
    codec_report.i2s_channels_ready = true;
    codec_report.i2s_channels_started = true;
    Check(!codec_report.hardware_ready(), "外部 Codec Profile 仍须等待控制器件 ACK");

    Esp32s3AudioProbe probe;
    const auto host_result = probe.Run(profile);
    Check(host_result.status.code == ErrorCode::kUnavailable,
          "主机不能伪装成 ESP32-S3 音频探针已执行");
    return 0;
}
