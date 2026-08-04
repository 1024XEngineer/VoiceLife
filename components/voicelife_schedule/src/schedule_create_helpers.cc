#include "schedule_create_helpers.h"

#include <algorithm>
#include <cctype>
#include <utility>

namespace voicelife::schedule {

std::string TrimScheduleText(std::string_view value) {
    const auto is_space = [](unsigned char character) { return std::isspace(character) != 0; };
    const auto first = std::find_if_not(value.begin(), value.end(), is_space);
    const auto last = std::find_if_not(value.rbegin(), value.rend(), is_space).base();
    return first < last ? std::string(first, last) : std::string{};
}

std::size_t ScheduleTextLength(std::string_view value) {
    return static_cast<std::size_t>(std::count_if(
        value.begin(), value.end(), [](unsigned char character) { return (character & 0xC0U) != 0x80U; }));
}

CreateScheduleResult InvalidCreateScheduleResult(std::string error) {
    return {
        .status = Status::Error(ErrorCode::kInvalidArgument, error),
        .message = {},
        .schedule = std::nullopt,
        .conflicts = {},
        .nearby_schedules = {},
        .error = std::move(error),
    };
}

}  // namespace voicelife::schedule
