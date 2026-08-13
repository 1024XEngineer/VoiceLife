#include "schedule_update_helpers.h"

#include <utility>

namespace voicelife::schedule {

UpdateScheduleResult InvalidUpdateScheduleResult(std::string error) {
    return {
        .status = Status::Error(ErrorCode::kInvalidArgument, error),
        .message = {},
        .schedule = std::nullopt,
        .conflicts = {},
        .error = std::move(error),
    };
}

}  // namespace voicelife::schedule
