#pragma once

#include <optional>
#include <string>

#include "voicelife/schedule/schedule_types.h"

namespace voicelife::schedule {

/// 创建日程所需的数据。
struct CreateScheduleCommand {
    std::string event;
    std::optional<DateTime> start_time;
    std::optional<DateTime> end_time;
    std::optional<std::string> location;
    std::optional<std::string> notes;
    bool ignore_conflict = false;
};

/// 删除日程所需的数据。
struct DeleteScheduleCommand {
    ScheduleId schedule_id = 0;
};

/// 修改日程所需的数据；未提供的可选字段保持原值。
struct UpdateScheduleCommand {
    ScheduleId schedule_id = 0;
    std::optional<std::string> event;
    std::optional<DateTime> start_time;
    std::optional<DateTime> end_time;
    std::optional<std::string> location;
    std::optional<std::string> notes;
    std::optional<ReminderId> reminder_id;
    bool ignore_conflict = false;
};

/// 查询日程所需的筛选和分页条件。
struct QueryScheduleCommand {
    std::optional<ScheduleId> schedule_id;
    std::optional<std::string> keyword;
    std::optional<DateTime> start_from;
    std::optional<DateTime> start_to;
    ScheduleStatusFilter status = ScheduleStatusFilter::kActive;
    int64_t limit = 10;
    int64_t offset = 0;
};

/// 写入日程操作记录所需的数据。
struct RecordScheduleOperationCommand {
    ScheduleOperationType type = ScheduleOperationType::kCreate;
    ScheduleId schedule_id = 0;
    std::string schedule_event;
    std::optional<JsonDocument> previous;
};

/// 撤销指定日程操作所需的数据。
struct UndoScheduleOperationCommand {
    OperationId operation_id = 0;
};

}  // namespace voicelife::schedule
