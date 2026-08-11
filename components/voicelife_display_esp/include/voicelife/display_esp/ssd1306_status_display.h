#pragma once

#include <string_view>

#include "voicelife/contracts/status.h"

namespace voicelife::display_esp {

/** @brief 初始化当前 PCB 的 128x32 I2C OLED 状态屏。 @return 初始化结果。 */
Status InitializeStatusDisplay();

/** @brief 在 OLED 上显示一个短 ASCII/中文状态词。 @param status 状态文本。 @return 绘制结果。 */
Status SetStatus(std::string_view status);

/**
 * @brief 在 OLED 上显示左侧牛头表情与右侧状态文本。
 * @param mood 表情键：neutral/happy/sad/thinking/surprised/speaking/angry。
 * @param text 右侧状态文本。
 * @return 绘制结果。
 */
Status SetEmotion(std::string_view mood, std::string_view text);

}  // namespace voicelife::display_esp
