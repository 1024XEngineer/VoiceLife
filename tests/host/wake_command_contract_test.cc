#include <cstdlib>
#include <cstring>
#include <iostream>

#include "voicelife/audio_esp/esp_multinet_wake_commands.h"

namespace {

void Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

}  // namespace

int main() {
    using voicelife::audio_esp::kMultiNetDetectionThreshold;
    using voicelife::audio_esp::kMultiNetWakeCommandCount;
    using voicelife::audio_esp::kMultiNetWakeCommands;

    Check(kMultiNetWakeCommandCount == 3, "本地命令数量应为三个");
    Check(std::strcmp(kMultiNetWakeCommands[0].grammar, "ni hao niu niu") == 0 &&
              std::strcmp(kMultiNetWakeCommands[0].display, "你好牛牛") == 0,
          "主唤醒词契约错误");
    Check(std::strcmp(kMultiNetWakeCommands[1].grammar, "niu lai") == 0 &&
              std::strcmp(kMultiNetWakeCommands[1].display, "牛来") == 0,
          "牛来唤醒词契约错误");
    Check(std::strcmp(kMultiNetWakeCommands[2].grammar, "bie shuo le") == 0 &&
              std::strcmp(kMultiNetWakeCommands[2].display, "别说了") == 0,
          "打断命令契约错误");
    for (std::size_t i = 0; i < kMultiNetWakeCommandCount; ++i) {
        Check(std::strcmp(kMultiNetWakeCommands[i].display, "牛牛") != 0, "已移除的独立牛牛命令不能重新注册");
    }
    Check(kMultiNetDetectionThreshold > 0.0f && kMultiNetDetectionThreshold < 1.0f,
          "MultiNet 阈值必须位于有效概率范围");
    return 0;
}
