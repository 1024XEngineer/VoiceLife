#pragma once

#include <string_view>

#include "voicelife/contracts/status.h"

namespace voicelife::display_esp {

/** @brief 初始化当前 PCB 的 128x32 I2C OLED 状态屏。 @return 初始化结果。 */
Status InitializeStatusDisplay();

/** @brief 在 OLED 上显示一个短 ASCII 状态词。 @param status 状态文本。 @return 绘制结果。 */
Status SetStatus(std::string_view status);

}  // namespace voicelife::display_esp
