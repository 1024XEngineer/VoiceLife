#pragma once

#include "voicelife/contracts/status.h"

namespace voicelife::runtime {

/** @brief Initializes and starts the embedded runtime. @return Runtime-start result. */
Status Start();

}  // namespace voicelife::runtime
