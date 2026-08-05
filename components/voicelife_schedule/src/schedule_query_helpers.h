#pragma once

#include <string_view>

#include "voicelife/schedule/schedule_commands.h"
#include "voicelife/schedule/schedule_results.h"

namespace voicelife::schedule {

/**
 * @brief 校验日程查询条件。
 * @param command 查询筛选和分页条件。
 * @return 参数合法时返回成功，否则返回参数错误。
 */
Status ValidateQueryScheduleCommand(const QueryScheduleCommand& command);

/**
 * @brief 判断日程是否满足查询的全部筛选条件。
 * @param schedule 待匹配的日程。
 * @param command 查询筛选条件。
 * @return 全部条件均匹配时返回 true。
 */
bool MatchesScheduleQuery(const Schedule& schedule, const QueryScheduleCommand& command);

/**
 * @brief 判断事件标题是否匹配关键词表达式。
 * @param event 日程事件标题。
 * @param keyword 以空白拆分的关键词，支持加号前缀的必选词写法。
 * @return 每个有效关键词均被标题包含时返回 true。
 */
bool MatchesScheduleKeyword(std::string_view event, std::string_view keyword);

}  // namespace voicelife::schedule
