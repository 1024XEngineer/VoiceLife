#pragma once

#include <array>
#include <cstdint>

#include "generated/font16_data.h"

namespace voicelife::display_esp {

/**
 * @brief 16x16 1bpp 字体查询器：从内嵌 VLF1 资产按码点二分查找字形。
 * 覆盖 6845 个常用码点；未命中返回空字形（由渲染器画占位）。
 */
class Font16Provider {
   public:
    /** @brief 按码点查找 32 字节字形（2 页 x 16 列，SSD1306 页序）。
     *  @param codepoint Unicode 码点。
     *  @param glyph 输出字形缓冲（32 字节）；未命中填零。
     *  @return true 表示命中。 */
    bool Lookup(uint32_t codepoint, std::array<uint8_t, 32>& glyph) const {
        glyph.fill(0);
        if (font16_data::kFontData[0] != 'V' || font16_data::kFontData[1] != 'L' || font16_data::kFontData[2] != 'F' ||
            font16_data::kFontData[3] != '1') {
            return false;
        }
        const uint32_t count = static_cast<uint32_t>(font16_data::kFontData[4]) |
                               (static_cast<uint32_t>(font16_data::kFontData[5]) << 8) |
                               (static_cast<uint32_t>(font16_data::kFontData[6]) << 16) |
                               (static_cast<uint32_t>(font16_data::kFontData[7]) << 24);
        constexpr size_t kEntry = font16_data::kEntryBytes;
        size_t lo = 0;
        size_t hi = count;
        while (lo < hi) {
            const size_t mid = (lo + hi) / 2;
            const size_t base = 8 + mid * kEntry;
            const uint32_t cp = static_cast<uint32_t>(font16_data::kFontData[base]) |
                                (static_cast<uint32_t>(font16_data::kFontData[base + 1]) << 8) |
                                (static_cast<uint32_t>(font16_data::kFontData[base + 2]) << 16) |
                                (static_cast<uint32_t>(font16_data::kFontData[base + 3]) << 24);
            if (cp == codepoint) {
                for (size_t i = 0; i < font16_data::kGlyphBytes; ++i) {
                    glyph[i] = font16_data::kFontData[base + 4 + i];
                }
                return true;
            }
            if (cp < codepoint) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        return false;
    }
};

}  // namespace voicelife::display_esp
