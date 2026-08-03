#include "voicelife/platform/sequential_id_generator.h"

#include <chrono>

namespace voicelife::platform {

std::string SequentialIdGenerator::Next(const char* prefix) {
    return std::string(prefix) + "-" + std::to_string(next_.fetch_add(1));
}

int64_t SequentialIdGenerator::Now() const {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

}  // namespace voicelife::platform
