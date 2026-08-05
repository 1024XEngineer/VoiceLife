#include "schedule_query_helpers.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>

namespace voicelife::schedule {
namespace {

/**
 * @brief 将 ASCII 字符转换为小写，同时保留 UTF-8 字节。
 * @param value 要规范化的文本。
 * @return 可用于英文不区分大小写匹配的文本。
 */
std::string NormalizeKeywordText(std::string_view value) {
    std::string normalized(value);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return normalized;
}

/**
 * @brief 判断日程状态是否满足状态筛选条件。
 * @param status 日程当前状态。
 * @param filter 查询状态筛选值。
 * @return 状态满足筛选条件时返回 true。
 */
bool MatchesStatus(ScheduleStatus status, ScheduleStatusFilter filter) {
    switch (filter) {
        case ScheduleStatusFilter::kAll:
            return true;
        case ScheduleStatusFilter::kActive:
            return status == ScheduleStatus::kActive;
        case ScheduleStatusFilter::kCancelled:
            return status == ScheduleStatus::kCancelled;
        case ScheduleStatusFilter::kCompleted:
            return status == ScheduleStatus::kCompleted;
    }
    return false;
}

}  // namespace

Status ValidateQueryScheduleCommand(const QueryScheduleCommand& command) {
    if (command.schedule_id.has_value() && *command.schedule_id <= 0) {
        return Status::Error(ErrorCode::kInvalidArgument, "日程 ID 必须大于 0");
    }
    if (command.start_from.has_value() && command.start_to.has_value() && *command.start_from > *command.start_to) {
        return Status::Error(ErrorCode::kInvalidArgument, "开始时间范围下限不能晚于上限");
    }
    if (command.limit <= 0 || command.limit > 50) {
        return Status::Error(ErrorCode::kInvalidArgument, "返回条数必须在 1 到 50 之间");
    }
    if (command.offset < 0) {
        return Status::Error(ErrorCode::kInvalidArgument, "分页偏移量不能小于 0");
    }
    return Status::Ok();
}

bool MatchesScheduleKeyword(std::string_view event, std::string_view keyword) {
    const std::string normalized_event = NormalizeKeywordText(event);
    std::istringstream stream{NormalizeKeywordText(keyword)};
    std::string token;
    while (stream >> token) {
        if (token.front() == '+') token.erase(0, 1);
        if (token.empty()) continue;
        if (normalized_event.find(token) == std::string::npos) return false;
    }
    return true;
}

bool MatchesScheduleQuery(const Schedule& schedule, const QueryScheduleCommand& command) {
    if (command.schedule_id.has_value() && schedule.id != *command.schedule_id) return false;
    if (!MatchesStatus(schedule.status, command.status)) return false;
    if (command.keyword.has_value() && !MatchesScheduleKeyword(schedule.event, *command.keyword)) return false;

    if (command.start_from.has_value() || command.start_to.has_value()) {
        if (!schedule.start_time.has_value()) return false;
        if (command.start_from.has_value() && *schedule.start_time < *command.start_from) return false;
        if (command.start_to.has_value() && *schedule.start_time > *command.start_to) return false;
    }
    return true;
}

}  // namespace voicelife::schedule
