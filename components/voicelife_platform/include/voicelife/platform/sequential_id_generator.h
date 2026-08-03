#pragma once

#include <atomic>

#include "voicelife/application/calendar_application.h"

namespace voicelife::platform {

class SequentialIdGenerator final : public application::IdGeneratorPort {
   public:
    std::string Next(const char* prefix) override;
    int64_t Now() const override;

   private:
    std::atomic<uint64_t> next_{1};
};

}  // namespace voicelife::platform
