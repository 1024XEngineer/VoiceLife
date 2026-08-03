#pragma once

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace voicelife::test {

inline void Check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL " << message << '\n';
        std::exit(1);
    }
}

}  // namespace voicelife::test
