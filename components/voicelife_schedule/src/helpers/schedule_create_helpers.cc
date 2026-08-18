#include "schedule_create_helpers.h"

#include <algorithm>
#include <cctype>
#include <utility>

namespace voicelife::schedule {

// 创建/修改日程前统一做文本清理和长度统计，避免调用方重复处理空白与 UTF-8 字符。
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

// 构造创建日程的参数错误结果，集中处理错误状态和空冲突/临近日程字段。
CreateScheduleResult InvalidCreateScheduleResult(std::string error) {
    const Status status = Status::Error(ErrorCode::kInvalidArgument, error);
    return {
        .result = CommandResult<std::optional<Schedule>>::Failure(status),
        .message = {},
        .conflicts = {},
        .nearby_schedules = {},
    };
}

}  // namespace voicelife::schedule
