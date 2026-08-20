#include "voicelife/schedule/schedule_service.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <optional>
#include <utility>

#include "../helpers/schedule_create_helpers.h"
#include "../helpers/schedule_query_helpers.h"
#include "../helpers/schedule_snapshot_helpers.h"
#include "../helpers/schedule_update_helpers.h"
#include "../rules/schedule_time_rules.h"
#include "voicelife/schedule/schedule_factory.h"
#include "voicelife/schedule/schedule_operation_service.h"

namespace voicelife::schedule {
namespace {

constexpr std::size_t kMaximumEventLength = 100;

/**
 * @brief 在变更成功后追加一条操作日志（方案 A：尽力而为，失败不回滚变更）。
 * @param service 可空的操作记录服务。
 * @param entity_type 被操作实体类型。
 * @param type 操作类型。
 * @param entity_id 被操作实体标识。
 * @param label 展示用名称。
 * @param before 操作前快照 JSON。
 * @return 无。
 */
void RecordScheduleMutation(ScheduleOperationService* service, OperationEntityType entity_type,
                            ScheduleOperationType type, int64_t entity_id, std::string label,
                            std::optional<std::string> before) {
    if (service == nullptr) return;
    (void)service->record_operation({
        .entity_type = entity_type,
        .type = type,
        .entity_id = entity_id,
        .label = std::move(label),
        .before = std::move(before),
    });
}

}  // namespace

ScheduleService::ScheduleService(ScheduleRepository& repository, ScheduleOperationService* operation_service)
    : repository_(repository), operation_service_(operation_service) {}

CreateScheduleResult ScheduleService::create_schedule(const CreateScheduleCommand& command) const {
    // 先清理文本并校验入参，避免把无效数据带入后续组装和落库。
    const std::string event = TrimScheduleText(command.event);
    if (event.empty()) return InvalidCreateScheduleResult("日程名称不能为空");
    if (ScheduleTextLength(event) > kMaximumEventLength) {
        return InvalidCreateScheduleResult("日程名称不能超过 100 个字符");
    }
    if (!command.start_time.has_value() && command.end_time.has_value()) {
        return InvalidCreateScheduleResult("日程提供结束时间时必须同时提供开始时间");
    }
    if (command.start_time.has_value() && command.end_time.has_value() && *command.end_time <= *command.start_time) {
        return InvalidCreateScheduleResult("日程结束时间必须晚于开始时间");
    }

    // 用校验后的数据组装领域实体，时间戳和 ID 留给持久化层生成。
    Schedule schedule = ScheduleFactory::CreateFromCommand(CreateScheduleCommand{
        .event = event,
        .start_time = command.start_time,
        .end_time = command.end_time,
        .location = command.location,
        .notes = command.notes,
        .ignore_conflict = command.ignore_conflict,
    });

    std::vector<Schedule> conflicts;
    std::vector<Schedule> nearby_schedules;
    if (schedule.start_time.has_value()) {
        // 只查询候选时间窗口，避免全量读取后再做时间过滤。
        const auto [window_start, window_end] = ScheduleNearbyWindow(schedule);
        const Result<std::vector<Schedule>> candidates =
            repository_.FindOverlapping(window_start, window_end, std::nullopt);
        if (!candidates.ok()) {
            return {
                .result = CommandResult<std::optional<Schedule>>::Failure(candidates.status),
                .message = {},
                .conflicts = {},
                .nearby_schedules = {},
            };
        }
        conflicts = FindConflictingSchedules(schedule, *candidates.value);
        nearby_schedules = FindNearbySchedules(schedule, *candidates.value);
    }

    // 有冲突且未显式忽略时，直接返回冲突信息，不进入落库流程。
    if (!conflicts.empty() && !command.ignore_conflict) {
        return {
            .result = CommandResult<std::optional<Schedule>>::Failure(
                Status::Error(ErrorCode::kConflict, "日程时间与已有日程冲突")),
            .message = {},
            .conflicts = std::move(conflicts),
            .nearby_schedules = std::move(nearby_schedules),
        };
    }

    // 写入仓储，并采用持久化层补全 ID、时间戳后的实体作为最终返回基础。
    const Result<Schedule> stored = repository_.Insert(schedule);
    if (!stored.ok()) {
        return {
            .result = CommandResult<std::optional<Schedule>>::Failure(stored.status),
            .message = {},
            .conflicts = std::move(conflicts),
            .nearby_schedules = std::move(nearby_schedules),
        };
    }
    schedule = *stored.value;

    // 变更成功后追加创建日志；kCreate 无 before 快照。
    RecordScheduleMutation(operation_service_, OperationEntityType::kSchedule, ScheduleOperationType::kCreate,
                           schedule.id, schedule.event, std::nullopt);

    // 组装成功结果，保留冲突和临近日程信息供调用方做提醒。
    const std::string message = nearby_schedules.empty() ? "日程创建成功" : "日程创建成功，附近还有其他日程";
    return {
        .result = CommandResult<std::optional<Schedule>>::Success(schedule),
        .message = message,
        .conflicts = std::move(conflicts),
        .nearby_schedules = std::move(nearby_schedules),
    };
}

CancelScheduleResult ScheduleService::cancel_schedule(const CancelScheduleCommand& command) {
    // 校验入参，拒绝非法 ID。
    if (command.schedule_id <= 0) {
        constexpr char kError[] = "日程 ID 必须为正整数";
        return {
            .result = CommandResult<bool>::Failure(Status::Error(ErrorCode::kInvalidArgument, kError)),
            .schedule_id = command.schedule_id,
        };
    }

    // 这里只负责已经落库的 schedule 数据；未落库的周期实例由 rule service 走 occurrence 操作。
    const Result<Schedule> loaded = repository_.FindById(command.schedule_id);
    if (!loaded.ok()) {
        return {
            .result = CommandResult<bool>::Failure(loaded.status),
            .schedule_id = command.schedule_id,
        };
    }

    // 委托仓储做软取消，保留历史数据供回滚参考。
    const Status cancelled = repository_.Delete(command.schedule_id);
    if (!cancelled.ok()) {
        return {
            .result = CommandResult<bool>::Failure(cancelled),
            .schedule_id = command.schedule_id,
        };
    }

    // 变更成功后追加取消日志；before 保存取消前的完整日程快照。
    RecordScheduleMutation(operation_service_, OperationEntityType::kSchedule, ScheduleOperationType::kDelete,
                           command.schedule_id, loaded.value->event, SerializeScheduleSnapshot(*loaded.value));

    return {
        .result = CommandResult<bool>::Success(true),
        .schedule_id = command.schedule_id,
    };
}

UpdateScheduleResult ScheduleService::update_schedule(const UpdateScheduleCommand& command) {
    // 校验 ID，并确认至少有一个字段需要更新。
    if (command.schedule_id <= 0) return InvalidUpdateScheduleResult("日程 ID 必须大于零");

    // 确认至少提供一个待修改字段，避免无意义的数据库读取和写入。
    const bool has_update = command.event.has_value() || command.start_time.has_value() ||
                            command.end_time.has_value() || command.location.has_value() || command.notes.has_value();
    if (!has_update) return InvalidUpdateScheduleResult("至少需要提供一个要修改的字段");

    // 从仓储读取目标，避免全量拉取后再线性查找。
    const Result<Schedule> loaded = repository_.FindById(command.schedule_id);
    if (!loaded.ok()) {
        return {
            .result = CommandResult<std::optional<Schedule>>::Failure(loaded.status),
            .message = {},
            .conflicts = {},
        };
    }
    // 基于最新日程构造更新后的实体，未提供的字段保持原值，双层 optional 用于表达显式清空。
    Schedule updated = *loaded.value;
    if (command.event.has_value()) {
        updated.event = TrimScheduleText(*command.event);
        if (updated.event.empty()) return InvalidUpdateScheduleResult("日程名称不能为空");
        if (ScheduleTextLength(updated.event) > kMaximumEventLength) {
            return InvalidUpdateScheduleResult("日程名称不能超过 100 个字符");
        }
    }
    ApplyNullableUpdate(command.start_time, updated.start_time);
    ApplyNullableUpdate(command.end_time, updated.end_time);
    ApplyNullableUpdate(command.location, updated.location);
    ApplyNullableUpdate(command.notes, updated.notes);

    // 合并后再做完整校验，避免只检查本次字段而漏掉旧字段导致的不合法组合。
    if (!updated.start_time.has_value() && updated.end_time.has_value()) {
        return InvalidUpdateScheduleResult("日程提供结束时间时必须同时提供开始时间");
    }
    if (updated.start_time.has_value() && updated.end_time.has_value() && *updated.end_time <= *updated.start_time) {
        return InvalidUpdateScheduleResult("日程结束时间必须晚于开始时间");
    }

    // 只对 active 且有开始时间的更新结果做冲突检测，查询时排除自身。
    std::vector<Schedule> conflicts;
    if (updated.status == ScheduleStatus::kActive && updated.start_time.has_value()) {
        const auto [window_start, window_end] = ScheduleNearbyWindow(updated);
        const Result<std::vector<Schedule>> candidates =
            repository_.FindOverlapping(window_start, window_end, updated.id);
        if (!candidates.ok()) {
            return {
                .result = CommandResult<std::optional<Schedule>>::Failure(candidates.status),
                .message = {},
                .conflicts = {},
            };
        }
        conflicts = FindConflictingSchedules(updated, *candidates.value);
    }
    if (!conflicts.empty() && !command.ignore_conflict) {
        const std::string error = "修改后的日程时间与已有日程冲突";
        return {
            .result = CommandResult<std::optional<Schedule>>::Failure(Status::Error(ErrorCode::kConflict, error)),
            .message = {},
            .conflicts = std::move(conflicts),
        };
    }

    // 更新时间戳并将完整日程写回仓储。
    updated.updated_at = std::chrono::time_point_cast<std::chrono::seconds>(std::chrono::system_clock::now());
    const Status stored = repository_.Update(updated);
    if (!stored.ok()) {
        return {
            .result = CommandResult<std::optional<Schedule>>::Failure(stored),
            .message = {},
            .conflicts = std::move(conflicts),
        };
    }

    // 变更成功后追加修改日志；before 保存更新前的完整日程快照。
    RecordScheduleMutation(operation_service_, OperationEntityType::kSchedule, ScheduleOperationType::kUpdate,
                           updated.id, updated.event, SerializeScheduleSnapshot(*loaded.value));

    // 忽略冲突时仍返回冲突列表，便于调用方提示潜在影响
    return {
        .result = CommandResult<std::optional<Schedule>>::Success(updated),
        .message = conflicts.empty() ? "日程修改成功" : "日程修改成功，已忽略时间冲突",
        .conflicts = std::move(conflicts),
    };
}

Status ScheduleService::complete_schedule(ScheduleId schedule_id) {
    if (schedule_id <= 0) {
        return Status::Error(ErrorCode::kInvalidArgument, "日程 ID 必须大于零");
    }

    const Result<Schedule> loaded = repository_.FindById(schedule_id);
    if (!loaded.ok()) return loaded.status;

    Schedule completed = *loaded.value;
    if (completed.status != ScheduleStatus::kActive) {
        return Status::Error(ErrorCode::kConflict, "只有进行中的日程可以标记为已完成");
    }

    completed.status = ScheduleStatus::kCompleted;
    completed.updated_at = std::chrono::time_point_cast<std::chrono::seconds>(std::chrono::system_clock::now());
    return repository_.Update(completed);
}

QueryScheduleResult ScheduleService::query_schedule(const QueryScheduleCommand& command) const {
    // 先校验查询条件，避免非法筛选和分页参数进入 SQL。
    const Status validation = ValidateQueryScheduleCommand(command);
    if (!validation.ok()) {
        return {.result = CommandResult<std::vector<Schedule>>::Failure(validation), .total = 0};
    }

    // 分页数据和总数都交给仓储按查询条件下推，服务层不再做全量过滤。
    const Result<std::vector<Schedule>> loaded = repository_.Find(command);
    if (!loaded.ok()) {
        return {.result = CommandResult<std::vector<Schedule>>::Failure(loaded.status), .total = 0};
    }
    const Result<int64_t> total = repository_.Count(command);
    if (!total.ok()) {
        return {.result = CommandResult<std::vector<Schedule>>::Failure(total.status), .total = 0};
    }
    return {.result = CommandResult<std::vector<Schedule>>::Success(*loaded.value), .total = *total.value};
}

}  // namespace voicelife::schedule
