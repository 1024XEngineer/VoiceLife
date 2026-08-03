#pragma once

#include "voicelife/timing/timing_task.h"

namespace voicelife::timing {

class RecurrencePolicy {
   public:
    Result<std::vector<int64_t>> Expand(const RecurrenceRule&, int64_t range_start, int64_t range_end) const;
    Result<int64_t> NextAfter(const RecurrenceRule&, int64_t occurrence_at) const;
    Status Validate(const RecurrenceRule&) const;
};

}  // namespace voicelife::timing
