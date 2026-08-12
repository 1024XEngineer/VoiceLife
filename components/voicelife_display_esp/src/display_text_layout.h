#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace voicelife::display_esp::text_layout {

/** @brief 单个 UTF-8 字符的显示码点与源字节宽度。 */
struct DecodedCodepoint {
    uint32_t codepoint = 0;
    std::size_t byte_width = 0;
};

/** @brief 解码首个 UTF-8 字符，并将全角 ASCII 标点归一化为半角。 */
DecodedCodepoint DecodeFirst(std::string_view text);
/** @brief 返回 OLED 16px 字体布局中的水平前进像素。 */
std::size_t Advance16(uint32_t codepoint);
/** @brief 跳过指定数量的 UTF-8 字符，返回对应的源字节偏移。 */
std::size_t ByteOffsetAfterCodepoints(std::string_view text, std::size_t codepoints);

}  // namespace voicelife::display_esp::text_layout
