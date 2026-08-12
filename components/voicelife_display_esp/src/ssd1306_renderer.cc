#include "ssd1306_renderer.h"

#include <algorithm>

#include "display_text_layout.h"

namespace voicelife::display_esp::ssd1306_renderer {
namespace {

void DrawGlyph16(FrameBuffer& buffer, int x, int page, const Glyph16& glyph) {
    for (int p = 0; p < 2; ++p) {
        if (page + p >= kPages) break;
        for (int column = 0; column < 16 && x + column < kWidth; ++column) {
            buffer[(page + p) * kWidth + x + column] |= glyph[p * 16 + column];
        }
    }
}

void DrawCodepoint16(FrameBuffer& buffer, int x, int page, uint32_t codepoint, Glyph16Resolver resolve_glyph) {
    const auto glyph = resolve_glyph(codepoint);
    const bool blank = std::all_of(glyph.begin(), glyph.end(), [](uint8_t byte) { return byte == 0; });
    if (!blank) DrawGlyph16(buffer, x, page, glyph);
}

void DrawAscii(FrameBuffer& buffer, int x, int page, char raw, AsciiGlyphResolver resolve_ascii) {
    const auto glyph = resolve_ascii(raw);
    for (int column = 0; column < 5 && x + column < kWidth; ++column) {
        buffer[page * kWidth + x + column] |= glyph[column];
    }
}

}  // namespace

void RenderText(FrameBuffer& buffer, std::string_view text, Glyph16Resolver resolve_glyph) {
    buffer.fill(0);
    int page = 1;
    int x = 0;
    size_t index = 0;
    while (index < text.size() && page < kPages) {
        if (text[index] == '\n') {
            ++index;
            x = 0;
            ++page;
            continue;
        }
        const auto decoded = text_layout::DecodeFirst(text.substr(index));
        const uint32_t codepoint = decoded.codepoint;
        const size_t advance = text_layout::Advance16(codepoint);
        if (advance >= 17) {
            if (x + 16 > kWidth) {
                x = 0;
                page += 2;
                if (page >= kPages) break;
            }
        } else if (x + 9 > kWidth) {
            x = 0;
            ++page;
            if (page >= kPages) break;
        }
        DrawCodepoint16(buffer, x, page, codepoint, resolve_glyph);
        x += advance;
        index += decoded.byte_width;
    }
}

void RenderEmotion(FrameBuffer& buffer, std::string_view mood, std::string_view status, std::string_view content,
                   size_t scroll_offset, MoodGlyphResolver resolve_mood, Glyph16Resolver resolve_glyph,
                   AsciiGlyphResolver resolve_ascii) {
    buffer.fill(0);
    DrawGlyph16(buffer, 2, 1, resolve_mood(mood));

    int x = 20;
    size_t index = 0;
    while (index < status.size()) {
        const auto decoded = text_layout::DecodeFirst(status.substr(index));
        const uint32_t codepoint = decoded.codepoint;
        if (codepoint >= 0x4e00 && codepoint <= 0x9fff) {
            const auto glyph = resolve_glyph(codepoint);
            const bool blank = std::all_of(glyph.begin(), glyph.end(), [](uint8_t byte) { return byte == 0; });
            if (!blank && x + 16 > kWidth) break;
            DrawGlyph16(buffer, x, 0, glyph);
            x += blank ? 16 : 17;
        } else {
            if (x + 6 > kWidth) break;
            DrawAscii(buffer, x, 0, static_cast<char>(codepoint), resolve_ascii);
            x += 6;
        }
        index += decoded.byte_width;
    }

    if (content.empty()) return;
    index = text_layout::ByteOffsetAfterCodepoints(content, scroll_offset);
    x = 20;
    while (index < content.size()) {
        const auto decoded = text_layout::DecodeFirst(content.substr(index));
        const uint32_t codepoint = decoded.codepoint;
        const size_t advance = text_layout::Advance16(codepoint);
        if (advance >= 17) {
            if (x + 16 > kWidth) break;
        } else if (x + 9 > kWidth) {
            break;
        }
        DrawCodepoint16(buffer, x, 2, codepoint, resolve_glyph);
        x += advance;
        index += decoded.byte_width;
    }
}

}  // namespace voicelife::display_esp::ssd1306_renderer
