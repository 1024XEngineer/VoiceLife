#pragma once

#ifdef ESP_PLATFORM

#include "voicelife/contracts/status.h"

namespace voicelife::runtime {

/** @brief 运行受控 PCM 采集和播放自检，仅供显式 SDKConfig 开关使用。 */
Status RunVoiceLifePcbAudioPortSmoke();

}  // namespace voicelife::runtime

#endif
