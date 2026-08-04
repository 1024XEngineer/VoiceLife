#pragma once

#include "voicelife/contracts/status.h"

namespace voicelife::runtime {

/** @brief 初始化并启动设备运行时。 @return 运行时启动结果。 */
Status Start();

}  // namespace voicelife::runtime
