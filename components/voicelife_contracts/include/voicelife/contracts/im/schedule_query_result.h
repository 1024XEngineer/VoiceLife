#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "im_contracts.h"
#include "voicelife/contracts/json.h"

namespace voicelife::contracts::im {

/// 设备侧日程查询的完整 IM 结果载荷。
struct ScheduleQueryResultIntent {
    std::string schemaVersion;
    std::string businessEventId;
    std::string correlationId;
    std::optional<std::string> userId;
    std::string deviceId;
    std::optional<std::string> keyword;
    std::string status;
    std::optional<std::string> startDate;
    std::optional<std::string> endDate;
    int64_t resultCount = 0;
    JsonValue schedules;
    JsonValue futureOccurrences;
    JsonValue exceptions;
    std::string queriedAt;
};

/**
 * @brief 解析并校验完整日程查询结果。
 * @param root 待解析的 JSON 根对象。
 * @param out 解析后的日程查询结果。
 * @return 解析成功或失败状态。
 */
Status ParseScheduleQueryResultIntent(const JsonValue& root, ScheduleQueryResultIntent& out);

}  // namespace voicelife::contracts::im
