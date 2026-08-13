#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>

namespace voicelife::schedule {

/**
 * @brief 将 ASCII 字符转换为小写，同时保留 UTF-8 字节。
 * @param value 要规范化的文本。
 * @return 可用于英文不区分大小写匹配的文本。
 */
inline std::string NormalizeKeywordTextForScore(std::string_view value) {
    std::string normalized(value);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return normalized;
}

/**
 * @brief 计算事件标题相对关键词的简单相关度评分。
 * @param event 日程事件标题。
 * @param keyword 查询关键词；为空或未命中标题时返回 0。
 * @return 完全相等 100，标题前缀 80，标题包含 60，其余 0。
 */
inline int64_t ScoreScheduleKeyword(std::string_view event, std::string_view keyword) {
    if (keyword.empty()) return 0;

    const std::string normalized_event = NormalizeKeywordTextForScore(event);
    const std::string normalized_keyword = NormalizeKeywordTextForScore(keyword);
    if (normalized_event == normalized_keyword) return 100;
    if (normalized_event.rfind(normalized_keyword, 0) == 0) return 80;
    if (normalized_event.find(normalized_keyword) != std::string::npos) return 60;
    return 0;
}

}  // namespace voicelife::schedule
