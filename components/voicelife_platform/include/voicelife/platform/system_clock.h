#pragma once

#include "voicelife/application/calendar_application.h"

namespace voicelife::platform {

class SystemClock final : public application::ClockPort {
   public:
    int64_t Now() const override;
};

}  // namespace voicelife::platform
