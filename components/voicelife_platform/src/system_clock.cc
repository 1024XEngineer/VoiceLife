#include "voicelife/platform/system_clock.h"

#include <chrono>

namespace voicelife::platform {

int64_t SystemClock::Now() const {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

}  // namespace voicelife::platform
