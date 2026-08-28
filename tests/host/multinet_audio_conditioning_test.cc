#include "voicelife/audio_esp/multinet_audio_conditioning.h"

#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

void Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

}  // namespace

int main() {
    using voicelife::audio_esp::ConditionMultiNetPcm;
    using voicelife::audio_esp::MultiNetAudioConditioningConfig;

    std::vector<int16_t> quiet(320, 80);
    const auto quiet_stats = ConditionMultiNetPcm(quiet.data(), quiet.size());
    Check(quiet_stats.gated && quiet_stats.gain_q10 == 1024, "噪声底以下的输入不能被放大");
    Check(quiet.front() == 80, "门控输入不得改变原始样本");

    std::vector<int16_t> quiet_speech(320, 900);
    const auto quiet_speech_stats = ConditionMultiNetPcm(quiet_speech.data(), quiet_speech.size());
    Check(!quiet_speech_stats.gated && quiet_speech_stats.gain_q10 > 1024 && quiet_speech_stats.gain_q10 <= 4096,
          "低响度语音应使用有界增益");
    Check(quiet_speech_stats.output_peak <= 30000, "conditioning 后峰值不得超过限幅");

    std::vector<int16_t> loud(320, 12000);
    const auto loud_stats = ConditionMultiNetPcm(loud.data(), loud.size());
    Check(loud_stats.gain_q10 < 1024, "过响输入应衰减到目标范围");
    Check(loud_stats.output_peak <= 30000, "过响输入也必须保留峰值余量");

    std::vector<int16_t> alternating(320);
    for (std::size_t index = 0; index < alternating.size(); ++index) {
        alternating[index] = index % 2 == 0 ? 32767 : -32768;
    }
    const auto clipped_stats = ConditionMultiNetPcm(alternating.data(), alternating.size());
    Check(clipped_stats.output_peak <= 30000, "满幅输入不得溢出");
    Check(clipped_stats.output_rms > 0, "非静音输入应保留有效信号");

    std::vector<int16_t> custom(320, 1000);
    MultiNetAudioConditioningConfig custom_config;
    custom_config.noise_floor_rms = 1200;
    const auto custom_stats = ConditionMultiNetPcm(custom.data(), custom.size(), custom_config);
    Check(custom_stats.gated, "自定义噪声底应生效");
    return 0;
}
