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

/// 取消一次性日程所需的数据。
struct CancelScheduleCommand {
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
    bool ignore_conflict = false;
};

/// 查询日程所需的筛选和分页条件。
struct QueryScheduleCommand {
    std::optional<ScheduleId> schedule_id;
    std::optional<ScheduleId> rule_id;
    std::optional<std::string> keyword;
    std::optional<DateTime> start_from;
    std::optional<DateTime> start_to;
    ScheduleStatusFilter status = ScheduleStatusFilter::kActive;
    int64_t limit = 10;
    int64_t offset = 0;
};

/// 写入一条日程操作记录所需的数据。
struct RecordOperationCommand {
    OperationEntityType entity_type = OperationEntityType::kSchedule;
    ScheduleOperationType type = ScheduleOperationType::kCreate;
    int64_t entity_id = 0;
    std::string label;
    /// 操作前快照 JSON；kCreate 必须为空，kUpdate / kDelete 必须有值。
    std::optional<std::string> before;
};

/// 查询操作记录所需的筛选和分页条件；与 QueryScheduleCommand 对齐。
struct QueryOperationCommand {
    std::optional<OperationId> operation_id; ///< 精确查单条，读 before 快照用
    std::optional<OperationEntityType> entity_type;
    std::optional<int64_t> entity_id;        ///< 需配合 entity_type
    std::optional<ScheduleOperationType> type;
    std::optional<DateTime> operated_from;   ///< 时间范围下界
    std::optional<DateTime> operated_to;     ///< 时间范围上界
    std::optional<std::string> keyword;      ///< label 模糊匹配
    int64_t limit = 20;
    int64_t offset = 0;
};

}  // namespace voicelife::schedule
