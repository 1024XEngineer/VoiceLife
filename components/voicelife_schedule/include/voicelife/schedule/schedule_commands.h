#pragma once

#include <optional>
#include <string>

#include "voicelife/schedule/schedule_types.h"

namespace voicelife::schedule {

/**
 * @brief 可清空字段的修改值；外层无值表示不修改，内层无值表示清空。
 * @tparam T 字段实际保存的数据类型。
 */
template <typename T>
using NullableScheduleUpdate = std::optional<std::optional<T>>;

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

/// 修改日程所需的数据；未提供的字段保持原值，可清空字段通过双层 optional 表达。
struct UpdateScheduleCommand {
    ScheduleId schedule_id = 0;
    std::optional<std::string> event;
    NullableScheduleUpdate<DateTime> start_time;
    NullableScheduleUpdate<DateTime> end_time;
    NullableScheduleUpdate<std::string> location;
    NullableScheduleUpdate<std::string> notes;
    NullableScheduleUpdate<ReminderId> reminder_id;
    std::optional<ScheduleStatus> status;
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

/// 写入日程操作记录所需的数据；撤销操作允许用空快照表达操作前日程不存在。
struct RecordScheduleOperationCommand {
    ScheduleOperationType type = ScheduleOperationType::kCreate;
    ScheduleId schedule_id = 0;
    std::string schedule_event;
    /// 创建时为空，修改和删除时保存完整快照，撤销时保存撤销前可能不存在的日程状态。
    std::optional<Schedule> previous;
};

/// 撤销指定日程操作所需的数据。
struct UndoScheduleOperationCommand {
    OperationId operation_id = 0;
};

}  // namespace voicelife::schedule
