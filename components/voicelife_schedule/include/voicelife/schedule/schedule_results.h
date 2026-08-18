#pragma once

#include <optional>
#include <string>
#include <vector>

#include "voicelife/contracts/status.h"
#include "voicelife/schedule/schedule_operation_repository.h"
#include "voicelife/schedule/schedule_types.h"

namespace voicelife::schedule {

/// 创建日程的返回数据。
struct CreateScheduleResult {
    CommandResult<std::optional<Schedule>> result;
    std::string message;
    std::vector<Schedule> conflicts;
    std::vector<Schedule> nearby_schedules;
};

/// 修改日程的返回数据。
struct UpdateScheduleResult {
    CommandResult<std::optional<Schedule>> result;
    std::string message;
    std::vector<Schedule> conflicts;
};

/// 取消日程的返回数据。
struct CancelScheduleResult {
    CommandResult<bool> result;
    ScheduleId schedule_id = 0;
};

/// 查询日程的返回数据，total 不受分页参数影响。
struct QueryScheduleResult {
    CommandResult<std::vector<Schedule>> result;
    int64_t total = 0;
};

/// 记录操作的返回数据。
struct RecordOperationResult {
    CommandResult<std::optional<OperationRecord>> result;
};

/// 查询操作的返回数据，total 不受分页影响。
struct QueryOperationResult {
    CommandResult<std::vector<OperationRecord>> result;
    int64_t total = 0;
};

}  // namespace voicelife::schedule
