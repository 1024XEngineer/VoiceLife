#pragma once

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

#include "voicelife/schedule/schedule_commands.h"
#include "voicelife/schedule/schedule_types.h"

namespace voicelife::test::in_memory_schedule_repository_helpers {

/** @brief 将关键词拆成空格分隔的词语。 @param text 被匹配文本。 @param keyword 原始关键词。 @return 命中时返回 true。
 */
inline bool MatchesKeyword(std::string_view text, std::string_view keyword) {
    std::string normalized_text(text);
    std::string normalized_keyword(keyword);
    std::transform(normalized_text.begin(), normalized_text.end(), normalized_text.begin(),
                   [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    std::transform(normalized_keyword.begin(), normalized_keyword.end(), normalized_keyword.begin(),
                   [](unsigned char character) { return static_cast<char>(std::tolower(character)); });

    std::istringstream stream{normalized_keyword};
    std::string token;
    while (stream >> token) {
        if (!token.empty() && token.front() == '+') token.erase(0, 1);
        if (token.empty()) continue;
        if (normalized_text.find(token) == std::string::npos) return false;
    }
    return true;
}

/**
 * @brief 计算下一条日程标识。
 * @param schedules 已有日程。
 * @return 大于全部已有标识的正整数。
 */
inline schedule::ScheduleId NextScheduleId(const std::vector<schedule::Schedule>& schedules) {
    schedule::ScheduleId next = 1;
    for (const schedule::Schedule& stored : schedules) next = std::max(next, stored.id + 1);
    return next;
}

/**
 * @brief 判断操作是否位于撤销窗口内。
 * @param operation 操作记录。
 * @param now 当前时间。
 * @return 操作时间位于闭区间时返回 true。
 */
inline bool IsWithinUndoWindow(const schedule::OperationRecord& operation, schedule::DateTime now) {
    return operation.operated_at >= now - std::chrono::minutes{15} && operation.operated_at <= now;
}

/** @brief 判断日程是否匹配查询条件。 @param schedule 日程。 @param query 查询条件。 @return 匹配时返回 true。 */
inline bool MatchesQuery(const schedule::Schedule& schedule, const schedule::QueryScheduleCommand& query) {
    if (query.schedule_id.has_value() && schedule.id != *query.schedule_id) return false;
    if (query.rule_id.has_value() && schedule.rule_id != query.rule_id) return false;
    if (query.status != schedule::ScheduleStatusFilter::kAll) {
        switch (query.status) {
            case schedule::ScheduleStatusFilter::kActive:
                if (schedule.status != schedule::ScheduleStatus::kActive) return false;
                break;
            case schedule::ScheduleStatusFilter::kCancelled:
                if (schedule.status != schedule::ScheduleStatus::kCancelled) return false;
                break;
            case schedule::ScheduleStatusFilter::kCompleted:
                if (schedule.status != schedule::ScheduleStatus::kCompleted) return false;
                break;
            case schedule::ScheduleStatusFilter::kAll:
                break;
        }
    }
    if (query.keyword.has_value() && !query.keyword->empty()) {
        const std::string& keyword = *query.keyword;
        if (!MatchesKeyword(schedule.event, keyword) &&
            (!schedule.location.has_value() || !MatchesKeyword(*schedule.location, keyword)) &&
            (!schedule.notes.has_value() || !MatchesKeyword(*schedule.notes, keyword))) {
            return false;
        }
    }
    if (query.start_from.has_value() || query.start_to.has_value()) {
        if (!schedule.start_time.has_value()) return false;
        if (query.start_from.has_value() && *schedule.start_time < *query.start_from) return false;
        if (query.start_to.has_value() && *schedule.start_time > *query.start_to) return false;
    }
    return true;
}

}  // namespace voicelife::test::in_memory_schedule_repository_helpers
