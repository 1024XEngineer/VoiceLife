#pragma once

#include <memory>

#include "voicelife/voice/voice_ports.h"

namespace voicelife::audio_esp {

/**
 * @brief 创建 SparkBot Linx 线上 Opus 策略。
 *
 * 本地音频边界仍为 PCM S16LE。策略只接受并产生在 Configure 中固定的
 * 线上 Opus Profile，避免 hello 宣告与实际二进制帧不一致。
 * @return 新建的 Opus 编解码策略。
 */
std::unique_ptr<voice::CodecStrategy> CreateEspOpusCodecStrategy();

}  // namespace voicelife::audio_esp
