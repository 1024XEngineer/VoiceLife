#include "voicelife/platform/sequential_id_generator.h"

namespace voicelife::platform {

std::string SequentialIdGenerator::Next(const char* prefix) {
    return std::string(prefix) + "-" + std::to_string(next_.fetch_add(1));
}

}  // namespace voicelife::platform
