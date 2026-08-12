#pragma once

#include <string_view>

#include "voicelife/contracts/status.h"

namespace voicelife::display_esp {

/** @brief 初始化当前 PCB 的 128x32 I2C OLED 状态屏。 @return 初始化结果。 */
Status InitializeStatusDisplay();

/** @brief 在 OLED 上显示一个短 ASCII/中文状态词。 @param status 状态文本。 @return 绘制结果。 */
Status SetStatus(std::string_view status);

/**
 * @brief 在 OLED 上显示左侧牛头表情、上行状态栏与下行内容栏。
 * @param mood 表情键：neutral/happy/sad/thinking/surprised/speaking/angry。
 * @param status 上行状态栏文本（短状态词）。
 * @param content 下行内容栏文本（用户语音/助手回复）；超宽时从 scroll_offset 字符滚动。
 * @param scroll_offset 下行滚动窗口起始字符（0=从头显示）。
 * @return 绘制结果。
 */
Status SetEmotion(std::string_view mood, std::string_view status, std::string_view content, size_t scroll_offset = 0);

}  // namespace voicelife::display_esp
