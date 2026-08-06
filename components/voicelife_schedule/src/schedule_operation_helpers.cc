#include "schedule_operation_helpers.h"

#include <cstddef>
#include <utility>

#include "schedule_create_helpers.h"

namespace voicelife::schedule {
namespace {

constexpr std::size_t kMaximumEventLength = 100;

/**
 * @brief 判断操作类型是否属于日程模块支持的范围。
 * @param type 待判断的操作类型。
 * @return 类型为创建、修改或删除时返回 true。
 */
bool IsSupportedScheduleOperationType(ScheduleOperationType type) {
    switch (type) {
        case ScheduleOperationType::kCreate:
        case ScheduleOperationType::kUpdate:
        case ScheduleOperationType::kDelete:
            return true;
    }
    return false;
}

}  // namespace

Status ValidateRecordScheduleOperationCommand(const RecordScheduleOperationCommand& command) {
    if (!IsSupportedScheduleOperationType(command.type)) {
        return Status::Error(ErrorCode::kInvalidArgument, "操作类型必须为创建、修改或删除");
    }
    if (command.schedule_id <= 0) {
        return Status::Error(ErrorCode::kInvalidArgument, "日程 ID 必须大于 0");
    }

    const std::string event = TrimScheduleText(command.schedule_event);
    if (event.empty()) {
        return Status::Error(ErrorCode::kInvalidArgument, "日程名称不能为空");
    }
    if (ScheduleTextLength(event) > kMaximumEventLength) {
        return Status::Error(ErrorCode::kInvalidArgument, "日程名称不能超过 100 个字符");
    }

    if (command.type == ScheduleOperationType::kCreate && command.previous.has_value()) {
        return Status::Error(ErrorCode::kInvalidArgument, "创建操作不能提供 previous 日程状态");
    }
    if (command.type != ScheduleOperationType::kCreate && !command.previous.has_value()) {
        return Status::Error(ErrorCode::kInvalidArgument, "修改和删除操作必须提供 previous 日程状态");
    }

    // previous 在领域层保留完整日程；未来由存储适配器序列化到数据库 JSON 字段。
    return Status::Ok();
}

RecordScheduleOperationResult InvalidRecordScheduleOperationResult(std::string error) {
    return {
        .status = Status::Error(ErrorCode::kInvalidArgument, error),
        .operation = std::nullopt,
        .error = std::move(error),
    };
}

}  // namespace voicelife::schedule
