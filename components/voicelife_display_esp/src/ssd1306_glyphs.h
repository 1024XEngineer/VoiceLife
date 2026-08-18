#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace voicelife::display_esp::internal {

std::array<uint8_t, 5> LookupAsciiGlyph(char value);
std::array<uint8_t, 32> LookupGlyph16(uint32_t codepoint);
std::array<uint8_t, 32> LookupMoodGlyph(std::string_view mood);

}  // namespace voicelife::display_esp::internal
