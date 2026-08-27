#pragma once

#include <cstddef>

namespace voicelife::audio_esp {

/** @brief MultiNet 中文命令的固定产品契约。拼音输入不携带声调。 */
struct MultiNetWakeCommand {
    int id;
    const char* grammar;
    const char* display;
};

inline constexpr float kMultiNetDetectionThreshold = 0.2f;

inline constexpr MultiNetWakeCommand kMultiNetWakeCommands[] = {
    {1, "ni hao niu niu", "你好牛牛"},
    {2, "niu lai", "牛来"},
    {3, "bie shuo le", "别说了"},
};

inline constexpr std::size_t kMultiNetWakeCommandCount = 3;

}  // namespace voicelife::audio_esp
