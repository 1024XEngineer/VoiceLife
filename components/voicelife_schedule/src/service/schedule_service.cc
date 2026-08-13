#include "voicelife/schedule/schedule_service.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <utility>

#include "../helpers/schedule_create_helpers.h"
#include "../helpers/schedule_operation_helpers.h"
#include "../helpers/schedule_operation_query_helpers.h"
#include "../helpers/schedule_query_helpers.h"
#include "../helpers/schedule_undo_helpers.h"
#include "../helpers/schedule_update_helpers.h"
#include "../mock/schedule_mock_data.h"
#include "../mock/schedule_operation_mock_data.h"
#include "../rules/schedule_time_rules.h"

namespace voicelife::schedule {
namespace {

constexpr std::size_t kMaximumEventLength = 100;

}  // namespace

ScheduleService::ScheduleService(ScheduleRepository& repository) : repository_(&repository) {}

CreateScheduleResult ScheduleService::create_schedule(const CreateScheduleCommand& command) const {
    // 健壮性校验
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
    if (command.idempotency_key.has_value() &&
        (command.idempotency_key->empty() || command.idempotency_key->size() > 128)) {
        return InvalidCreateScheduleResult("日程创建键无效");
    }

    // 重试必须优先回放既有结果，不能被随后创建的冲突日程改变语义。
    if (repository_ != nullptr && command.idempotency_key.has_value()) {
        const Result<std::optional<Schedule>> existing = repository_->FindByIdempotencyKey(*command.idempotency_key);
        if (!existing.ok()) {
            return {.status = existing.status,
                    .message = {},
                    .schedule = std::nullopt,
                    .conflicts = {},
                    .nearby_schedules = {},
                    .error = "读取已创建日程失败：" + existing.status.message,
                    .idempotent_replay = false};
        }
        if (existing.value->has_value()) {
            return {.status = Status::Ok(),
                    .message = "日程创建成功",
                    .schedule = std::move(**existing.value),
                    .conflicts = {},
                    .nearby_schedules = {},
                    .error = {},
                    .idempotent_replay = true};
        }
    }

    // 组装日程
    Schedule schedule{
        .id = 0,
        .event = event,
        .start_time = command.start_time,
        .end_time = command.end_time,
        .location = command.location,
        .notes = command.notes,
        .rule_id = std::nullopt,
        .status = ScheduleStatus::kActive,
        .created_at = {},
        .updated_at = {},
    };

    // 从注入的仓储读取现有日程；无仓储时保留旧单测使用的模拟数据。
    std::vector<Schedule> existing_schedules;
    if (repository_ != nullptr) {
        const Result<std::vector<Schedule>> loaded = repository_->FindAll();
        if (!loaded.ok()) {
            const std::string error = "读取现有日程失败：" + loaded.status.message;
            return {
                .status = loaded.status,
                .message = {},
                .schedule = std::nullopt,
                .conflicts = {},
                .nearby_schedules = {},
                .error = error,
            };
        }
        existing_schedules = *loaded.value;
    } else {
        existing_schedules = LoadMockSchedulesForCreate();
    }

    // 搜集与当前日程冲突日程和临近日程。
    std::vector<Schedule> conflicts;
    std::vector<Schedule> nearby_schedules;
    if (schedule.start_time.has_value()) {
        for (const Schedule& existing : existing_schedules) {
            if (existing.status != ScheduleStatus::kActive || !existing.start_time.has_value()) continue;
            if (SchedulesConflict(schedule, existing)) {
                conflicts.push_back(existing);
            } else if (SchedulesAreNearby(schedule, existing)) {
                nearby_schedules.push_back(existing);
            }
        }
    }

    // 日程是否冲突
    if (!conflicts.empty() && !command.ignore_conflict) {
        return {
            .status = Status::Error(ErrorCode::kConflict, "日程时间与已有日程冲突"),
            .message = {},
            .schedule = std::nullopt,
            .conflicts = std::move(conflicts),
            .nearby_schedules = std::move(nearby_schedules),
            .error = "日程时间与已有日程冲突",
        };
    }

    if (repository_ != nullptr) {
        const Result<Schedule> stored = command.idempotency_key.has_value()
                                            ? repository_->InsertOnce(schedule, *command.idempotency_key)
                                            : repository_->Insert(schedule);
        if (!stored.ok()) {
            const std::string error = "保存日程失败：" + stored.status.message;
            return {
                .status = stored.status,
                .message = {},
                .schedule = std::nullopt,
                .conflicts = std::move(conflicts),
                .nearby_schedules = std::move(nearby_schedules),
                .error = error,
            };
        }
        schedule = *stored.value;
    }

    const std::string message = nearby_schedules.empty() ? "日程创建成功" : "日程创建成功，附近还有其他日程";
    return {
        .status = Status::Ok(),
        .message = message,
        .schedule = std::move(schedule),
        .conflicts = std::move(conflicts),
        .nearby_schedules = std::move(nearby_schedules),
        .error = {},
        .idempotent_replay = false,
    };
}

DeleteScheduleResult ScheduleService::delete_schedule(const DeleteScheduleCommand& command) {
    // 健壮性校验
    if (command.schedule_id <= 0) {
        constexpr char kError[] = "日程 ID 必须为正整数";
        return {
            .status = Status::Error(ErrorCode::kInvalidArgument, kError),
            .schedule_id = command.schedule_id,
            .deleted = false,
            .error = kError,
        };
    }

    // 当前只搭建创建和查询的 SQLite 纵向链路，取消仍使用既有模拟存储。
    const Result<Schedule> cancelled = CancelMockSchedule(command.schedule_id);
    if (!cancelled.ok()) {
        return {
            .status = cancelled.status,
            .schedule_id = command.schedule_id,
            .deleted = false,
            .error = cancelled.status.message,
        };
    }

    return {
        .status = Status::Ok(),
        .schedule_id = command.schedule_id,
        .deleted = true,
        .error = {},
    };
}

UpdateScheduleResult ScheduleService::update_schedule(const UpdateScheduleCommand& command) {
    // 健壮性校验
    if (command.schedule_id <= 0) return InvalidUpdateScheduleResult("日程 ID 必须大于零");

    // 当前只搭建创建和查询的 SQLite 纵向链路，修改仍使用既有模拟存储。
    std::vector<Schedule> schedules = LoadMockSchedules();
    auto target = schedules.end();
    for (auto current = schedules.begin(); current != schedules.end(); ++current) {
        if (current->id == command.schedule_id) {
            target = current;
            break;
        }
    }
    if (target == schedules.end()) {
        const std::string error = "未找到要修改的日程";
        return {
            .status = Status::Error(ErrorCode::kNotFound, error),
            .message = {},
            .schedule = std::nullopt,
            .conflicts = {},
            .error = error,
        };
    }

    // 确认至少提供一个待修改字段
    const bool has_update = command.event.has_value() || command.start_time.has_value() ||
                            command.end_time.has_value() || command.location.has_value() || command.notes.has_value() ||
                            command.rule_id.has_value() || command.status.has_value();
    if (!has_update) return InvalidUpdateScheduleResult("至少需要提供一个要修改的字段");

    // 组装修改后的日程，未提供的字段保持不变，显式空值用于清空字段
    Schedule updated = *target;
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
    ApplyNullableUpdate(command.rule_id, updated.rule_id);
    if (command.status.has_value()) {
        if (!IsSupportedScheduleStatus(*command.status)) return InvalidUpdateScheduleResult("不支持的日程状态");
        updated.status = *command.status;
    }

    // 完整校验合并后的日程，避免增量校验遗漏原有字段约束
    if (!updated.start_time.has_value() && updated.end_time.has_value()) {
        return InvalidUpdateScheduleResult("日程提供结束时间时必须同时提供开始时间");
    }
    if (updated.start_time.has_value() && updated.end_time.has_value() && *updated.end_time <= *updated.start_time) {
        return InvalidUpdateScheduleResult("日程结束时间必须晚于开始时间");
    }

    // 搜集冲突日程；仅有效且有开始时间的日程参与检测，并排除自身
    std::vector<Schedule> conflicts;
    if (updated.status == ScheduleStatus::kActive && updated.start_time.has_value()) {
        for (const Schedule& existing : schedules) {
            if (existing.id == updated.id || existing.status != ScheduleStatus::kActive ||
                !existing.start_time.has_value()) {
                continue;
            }
            if (SchedulesConflict(updated, existing)) conflicts.push_back(existing);
        }
    }
    if (!conflicts.empty() && !command.ignore_conflict) {
        const std::string error = "修改后的日程时间与已有日程冲突";
        return {
            .status = Status::Error(ErrorCode::kConflict, error),
            .message = {},
            .schedule = std::nullopt,
            .conflicts = std::move(conflicts),
            .error = error,
        };
    }

    // 更新修改时间并准备持久化
    updated.updated_at = std::chrono::time_point_cast<std::chrono::seconds>(std::chrono::system_clock::now());

    // TODO：后续由日程仓储实现修改写入，本次只验证 INSERT 和 SELECT 链路。

    // 忽略冲突时仍返回冲突列表，便于调用方提示潜在影响
    return {
        .status = Status::Ok(),
        .message = conflicts.empty() ? "日程修改成功" : "日程修改成功，已忽略时间冲突",
        .schedule = std::move(updated),
        .conflicts = std::move(conflicts),
        .error = {},
    };
}

QueryScheduleResult ScheduleService::query_schedule(const QueryScheduleCommand& command) const {
    // 校验筛选条件和分页参数
    const Status validation = ValidateQueryScheduleCommand(command);
    if (!validation.ok()) {
        return {.status = validation, .schedules = {}, .total = 0, .error = validation.message};
    }

    // 先从仓储读取，再由领域规则完成筛选；SQL 文本不会进入服务层。
    std::vector<Schedule> matches;
    std::vector<Schedule> stored_schedules;
    if (repository_ != nullptr) {
        const Result<std::vector<Schedule>> loaded = repository_->FindAll();
        if (!loaded.ok()) {
            return {.status = loaded.status, .schedules = {}, .total = 0, .error = loaded.status.message};
        }
        stored_schedules = *loaded.value;
    } else {
        stored_schedules = LoadMockSchedulesForQuery();
    }
    for (const Schedule& schedule : stored_schedules) {
        if (MatchesScheduleQuery(schedule, command)) matches.push_back(schedule);
    }

    // 按开始时间和日程 ID 排序，无开始时间的日程排在末尾
    std::sort(matches.begin(), matches.end(), [](const Schedule& left, const Schedule& right) {
        if (left.start_time != right.start_time) {
            if (!left.start_time.has_value()) return false;
            if (!right.start_time.has_value()) return true;
            return *left.start_time < *right.start_time;
        }
        return left.id < right.id;
    });

    // 计算总数并截取当前分页
    const int64_t total = static_cast<int64_t>(matches.size());
    const std::size_t begin = command.offset >= total ? matches.size() : static_cast<std::size_t>(command.offset);
    const std::size_t count = std::min(static_cast<std::size_t>(command.limit), matches.size() - begin);
    std::vector<Schedule> page(matches.begin() + static_cast<std::ptrdiff_t>(begin),
                               matches.begin() + static_cast<std::ptrdiff_t>(begin + count));
    return {.status = Status::Ok(), .schedules = std::move(page), .total = total, .error = {}};
}

RecordScheduleOperationResult ScheduleService::record_schedule_operation(
    const RecordScheduleOperationCommand& command) {
    // 健壮性校验
    const Status validation = ValidateRecordScheduleOperationCommand(command);
    if (!validation.ok()) return InvalidRecordScheduleOperationResult(validation.message);

    // 组装操作记录实体
    OperationRecord operation{
        .id = 0,
        .type = command.type,
        .schedule_id = command.schedule_id,
        .schedule_event = TrimScheduleText(command.schedule_event),
        .operated_at = {},
        .previous = command.previous,
    };

    // 写入模拟操作记录
    const Result<OperationRecord> recorded = AppendMockScheduleOperation(std::move(operation));
    if (!recorded.ok()) {
        return {
            .status = recorded.status,
            .operation = std::nullopt,
            .error = recorded.status.message,
        };
    }

    return {
        .status = Status::Ok(),
        .operation = recorded.value,
        .error = {},
    };
}

QueryRecentScheduleOperationResult ScheduleService::query_recent_schedule_operation() const {
    // 获取当前时间，作为十五分钟窗口的结束边界
    const DateTime now = std::chrono::time_point_cast<std::chrono::seconds>(std::chrono::system_clock::now());

    // 查询近期可撤销操作；真实存储接入用户上下文后由存储层完成筛选
    std::vector<OperationRecord> operations = FilterRecentScheduleOperations(LoadMockScheduleOperations(), now);
    return {
        .status = Status::Ok(),
        .operations = std::move(operations),
        .error = {},
    };
}

UndoScheduleOperationResult ScheduleService::undo_schedule_operation(const UndoScheduleOperationCommand& command) {
    // 健壮性校验
    const Status validation = ValidateUndoScheduleOperationCommand(command);
    if (!validation.ok()) return FailedUndoScheduleOperationResult(validation);

    // 查找窗口内仍可撤销的目标操作
    const DateTime now = std::chrono::time_point_cast<std::chrono::seconds>(std::chrono::system_clock::now());
    const Result<OperationRecord> target = FindUndoableMockScheduleOperation(command.operation_id, now);
    if (!target.ok()) return FailedUndoScheduleOperationResult(target.status);

    // TODO：真实存储接入后，在同一事务内完成日程恢复、目标操作失效（不能再撤销至此）和撤销记录写入
    const Result<AppliedScheduleUndo> applied_result = ApplyMockScheduleUndo(*target.value);
    if (!applied_result.ok()) return FailedUndoScheduleOperationResult(applied_result.status);
    const AppliedScheduleUndo& applied = *applied_result.value;

    const std::string event = applied.before.has_value()
                                  ? applied.before->event
                                  : (applied.after.has_value() ? applied.after->event : target.value->schedule_event);
    // 组装撤销操作记录
    OperationRecord undo_operation{
        .id = 0,
        .type = ScheduleOperationType::kUndo,
        .schedule_id = target.value->schedule_id,
        .schedule_event = event,
        .operated_at = {},
        .previous = applied.before,
    };

    // 提交撤销记录并使目标操作失效
    const Result<OperationRecord> recorded =
        InvalidateMockScheduleOperationAndAppendUndo(target.value->id, std::move(undo_operation), now);
    if (!recorded.ok()) return FailedUndoScheduleOperationResult(recorded.status);

    // 撤销成功，返回原操作和恢复后的日程
    return {
        .status = Status::Ok(),
        .undone = true,
        .operation = target.value,
        .schedule = applied.after,
        .error = {},
    };
}

}  // namespace voicelife::schedule
