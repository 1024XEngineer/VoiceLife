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

}  // namespace

int main() {
    TestTextStartsAtPageOne();
    TestEmotionKeepsLegacyLayout();
    TestEmotionScrollCountsCodepoints();
    return 0;
}
