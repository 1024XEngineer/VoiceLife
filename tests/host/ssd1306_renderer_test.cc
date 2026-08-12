#include "ssd1306_renderer.h"

#include <array>
#include <cassert>
#include <cstdint>

namespace {

using voicelife::display_esp::ssd1306_renderer::AsciiGlyph;
using voicelife::display_esp::ssd1306_renderer::FrameBuffer;
using voicelife::display_esp::ssd1306_renderer::Glyph16;
namespace renderer = voicelife::display_esp::ssd1306_renderer;

Glyph16 ResolveGlyph(uint32_t codepoint) {
    Glyph16 glyph{};
    if (codepoint == 0) return glyph;
    glyph.fill(static_cast<uint8_t>(codepoint & 0xff));
    return glyph;
}

Glyph16 ResolveBlankCjk(uint32_t codepoint) {
    Glyph16 glyph{};
    if (codepoint >= 0x4e00 && codepoint <= 0x9fff) return glyph;
    glyph.fill(static_cast<uint8_t>(codepoint & 0xff));
    return glyph;
}

Glyph16 ResolveMood(std::string_view mood) {
    Glyph16 glyph{};
    glyph.fill(mood == "happy" ? 0xa5 : 0x5a);
    return glyph;
}

AsciiGlyph ResolveAscii(char raw) {
    AsciiGlyph glyph{};
    glyph.fill(static_cast<uint8_t>(raw));
    return glyph;
}

void TestTextStartsAtPageOne() {
    FrameBuffer buffer{};
    renderer::RenderText(buffer, "A", ResolveGlyph);
    assert(buffer[renderer::kWidth] == 'A');
    assert(buffer[0] == 0);
}

void TestEmotionKeepsLegacyLayout() {
    FrameBuffer buffer{};
    renderer::RenderEmotion(buffer, "happy", "A", "B", 0, ResolveMood, ResolveGlyph, ResolveAscii);

    assert(buffer[renderer::kWidth + 2] == 0xa5);
    assert(buffer[20] == 'A');
    assert(buffer[2 * renderer::kWidth + 20] == 'B');
}

void TestEmotionScrollCountsCodepoints() {
    FrameBuffer buffer{};
    renderer::RenderEmotion(buffer, "neutral", "",
                            "A\xE4\xB8\xAD"
                            "B",
                            2, ResolveMood, ResolveGlyph, ResolveAscii);

    assert(buffer[2 * renderer::kWidth + 20] == 'B');
}

void TestTextHandlesNewlinesWrappingAndBlankGlyphs() {
    FrameBuffer buffer{};
    renderer::RenderText(buffer, "A\nB", ResolveGlyph);
    assert(buffer[renderer::kWidth] == 'A');
    assert(buffer[2 * renderer::kWidth] == static_cast<uint8_t>('A' | 'B'));

    renderer::RenderText(buffer, std::string_view("\0", 1), ResolveGlyph);
    for (const auto byte : buffer) assert(byte == 0);

    renderer::RenderText(buffer, "牛牛牛牛牛牛牛牛牛牛", ResolveGlyph);
    renderer::RenderText(buffer, "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", ResolveGlyph);
}

void TestEmotionHandlesEmptyAndOverflowingRegions() {
    FrameBuffer buffer{};
    renderer::RenderEmotion(buffer, "neutral", "", "", 0, ResolveMood, ResolveGlyph, ResolveAscii);
    assert(buffer[renderer::kWidth + 2] == 0x5a);

    renderer::RenderEmotion(buffer, "neutral", "AAAAAAAAAAAAAAAAAAAA", "", 0, ResolveMood, ResolveGlyph, ResolveAscii);
    renderer::RenderEmotion(buffer, "neutral", "\xE7\x89\x9B", "", 0, ResolveMood, ResolveBlankCjk, ResolveAscii);
    renderer::RenderEmotion(buffer, "neutral", "", "AAAAAAAAAAAAAAAAAAAA", 0, ResolveMood, ResolveGlyph, ResolveAscii);
    renderer::RenderEmotion(buffer, "neutral", "", "牛牛牛牛牛牛牛牛牛牛", 0, ResolveMood, ResolveGlyph, ResolveAscii);
    renderer::RenderEmotion(buffer, "neutral", "", "A牛B", 99, ResolveMood, ResolveGlyph, ResolveAscii);
}

}  // namespace

int main() {
    TestTextStartsAtPageOne();
    TestEmotionKeepsLegacyLayout();
    TestEmotionScrollCountsCodepoints();
    TestTextHandlesNewlinesWrappingAndBlankGlyphs();
    TestEmotionHandlesEmptyAndOverflowingRegions();
    return 0;
}
