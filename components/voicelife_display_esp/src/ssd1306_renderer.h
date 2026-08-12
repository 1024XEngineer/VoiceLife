#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace voicelife::display_esp::ssd1306_renderer {

inline constexpr int kWidth = 128;
inline constexpr int kHeight = 32;
inline constexpr int kPages = kHeight / 8;

using FrameBuffer = std::array<uint8_t, kWidth * kPages>;
using Glyph16 = std::array<uint8_t, 32>;
using AsciiGlyph = std::array<uint8_t, 5>;
using Glyph16Resolver = Glyph16 (*)(uint32_t codepoint);
using MoodGlyphResolver = Glyph16 (*)(std::string_view mood);
using AsciiGlyphResolver = AsciiGlyph (*)(char value);

// These functions only compose a 128x32 SSD1306 page buffer. Panel I/O and
// glyph lookup policy deliberately remain in the board-specific adapter.
void RenderText(FrameBuffer& buffer, std::string_view text, Glyph16Resolver resolve_glyph);
void RenderEmotion(FrameBuffer& buffer, std::string_view mood, std::string_view status, std::string_view content,
                   size_t scroll_offset, MoodGlyphResolver resolve_mood, Glyph16Resolver resolve_glyph,
                   AsciiGlyphResolver resolve_ascii);

}  // namespace voicelife::display_esp::ssd1306_renderer
