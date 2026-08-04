#pragma once

#include <optional>
#include <string>
#include <vector>

#include "voicelife/contracts/status.h"
#include "voicelife/schedule/schedule_types.h"

namespace voicelife::schedule {

/// 创建日程的返回数据。
struct CreateScheduleResult {
    Status status;
    std::string message;
    std::optional<Schedule> schedule;
    std::vector<Schedule> conflicts;
    std::string error;
};

/// 修改日程的返回数据。
struct UpdateScheduleResult {
    Status status;
    std::string message;
    std::optional<Schedule> schedule;
    std::vector<Schedule> conflicts;
    std::string error;
};

/// 删除日程的返回数据。
struct DeleteScheduleResult {
    Status status;
    ScheduleId schedule_id = 0;
    bool deleted = false;
    std::string error;
};

/// 查询日程的返回数据，total 不受分页参数影响。
struct QueryScheduleResult {
    Status status;
    std::vector<Schedule> schedules;
    int64_t total = 0;
    std::string error;
};

/// 记录日程操作的返回数据。
struct RecordScheduleOperationResult {
    Status status;
    std::optional<OperationRecord> operation;
    std::string error;
};

/// 查询最近日程操作的返回数据。
struct QueryRecentScheduleOperationResult {
    Status status;
    std::vector<OperationRecord> operations;
    std::string error;
};

/// 撤销日程操作的返回数据。
struct UndoScheduleOperationResult {
    Status status;
    bool undone = false;
    std::optional<OperationRecord> operation;
    std::optional<Schedule> schedule;
    std::string error;
};

}  // namespace voicelife::schedule
