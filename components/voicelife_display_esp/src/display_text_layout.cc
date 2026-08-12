#include "display_text_layout.h"

namespace voicelife::display_esp::text_layout {

DecodedCodepoint DecodeFirst(std::string_view text) {
    if (text.empty()) return {};
    const uint8_t first = static_cast<uint8_t>(text[0]);
    if (first < 0x80U) return {.codepoint = first, .byte_width = 1};
    if ((first & 0xe0U) == 0xc0U && text.size() >= 2) {
        return {.codepoint = static_cast<uint32_t>(((first & 0x1fU) << 6) | (static_cast<uint8_t>(text[1]) & 0x3fU)),
                .byte_width = 2};
    }
    if ((first & 0xf0U) == 0xe0U && text.size() >= 3) {
        const uint32_t codepoint =
            static_cast<uint32_t>(((first & 0x0fU) << 12) | ((static_cast<uint8_t>(text[1]) & 0x3fU) << 6) |
                                  (static_cast<uint8_t>(text[2]) & 0x3fU));
        return {.codepoint = codepoint >= 0xff01U && codepoint <= 0xff5eU ? codepoint - 0xff01U + 0x21U : codepoint,
                .byte_width = 3};
    }
    if ((first & 0xf8U) == 0xf0U && text.size() >= 4) {
        return {.codepoint = static_cast<uint32_t>(
                    ((first & 0x07U) << 18) | ((static_cast<uint8_t>(text[1]) & 0x3fU) << 12) |
                    ((static_cast<uint8_t>(text[2]) & 0x3fU) << 6) | (static_cast<uint8_t>(text[3]) & 0x3fU)),
                .byte_width = 4};
    }
    return {.codepoint = 0, .byte_width = 1};
}

std::size_t Advance16(uint32_t codepoint) {
    if ((codepoint >= 0x4e00U && codepoint <= 0x9fffU) || (codepoint >= 0x3000U && codepoint <= 0x303fU) ||
        (codepoint >= 0xff00U && codepoint <= 0xffefU)) {
        return 17;
    }
    return 9;
}

std::size_t ByteOffsetAfterCodepoints(std::string_view text, std::size_t codepoints) {
    std::size_t offset = 0;
    for (std::size_t skipped = 0; offset < text.size() && skipped < codepoints; ++skipped) {
        const std::size_t width = DecodeFirst(text.substr(offset)).byte_width;
        offset += width == 0 ? 1 : width;
    }
    return offset;
}

}  // namespace voicelife::display_esp::text_layout
