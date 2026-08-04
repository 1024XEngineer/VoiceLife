#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

#include "voicelife/contracts/json.h"

namespace voicelife::schedule {

/// 日程、操作记录和提醒使用数据库兼容的 64 位整数标识。
using ScheduleId = int64_t;
using OperationId = int64_t;
using ReminderId = int64_t;
/// 模块内部的日期时间精确到秒；模型字符串与该类型的转换由边界适配器负责。
using DateTime = std::chrono::time_point<std::chrono::system_clock, std::chrono::seconds>;

/// 日程持久化状态。
enum class ScheduleStatus { kActive = 1, kCancelled = 2 };

/// 日程查询使用的状态筛选条件。
enum class ScheduleStatusFilter { kAll, kActive, kCancelled };

/// 可记录和撤销的日程操作类型。
enum class ScheduleOperationType { kCreate = 1, kUpdate = 2, kDelete = 3 };

/// 日程实体，对应 Schedule 数据表。
struct Schedule {
    ScheduleId id = 0;
    std::string event;
    std::optional<DateTime> start_time;
    std::optional<DateTime> end_time;
    std::optional<std::string> location;
    std::optional<std::string> notes;
    std::optional<ReminderId> reminder_id;
    ScheduleStatus status = ScheduleStatus::kActive;
    DateTime created_at;
    DateTime updated_at;
};

/// 日程操作记录，对应 OperationRecord 数据表。
struct OperationRecord {
    OperationId id = 0;
    ScheduleOperationType type = ScheduleOperationType::kCreate;
    ScheduleId schedule_id = 0;
    std::string schedule_event;
    DateTime operated_at;
    std::optional<JsonDocument> previous;
};

}  // namespace voicelife::schedule
