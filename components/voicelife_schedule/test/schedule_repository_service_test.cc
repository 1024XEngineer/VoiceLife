#include <chrono>
#include <cstdint>
#include <utility>
#include <vector>

#include "support/test_support.h"
#include "voicelife/schedule/schedule_operation_repository.h"
#include "voicelife/schedule/schedule_repository.h"
#include "voicelife/schedule/schedule_service.h"

using voicelife::ErrorCode;
using voicelife::Result;
using voicelife::schedule::CancelScheduleCommand;
using voicelife::schedule::CreateScheduleCommand;
using voicelife::schedule::DateTime;
using voicelife::schedule::OperationId;
using voicelife::schedule::OperationRecord;
using voicelife::schedule::QueryOperationCommand;
using voicelife::schedule::QueryScheduleCommand;
using voicelife::schedule::Schedule;
using voicelife::schedule::ScheduleId;
using voicelife::schedule::ScheduleOperationRepository;
using voicelife::schedule::ScheduleRepository;
using voicelife::schedule::ScheduleService;
using voicelife::schedule::ScheduleStatus;
using voicelife::schedule::ScheduleStatusFilter;
using voicelife::schedule::UpdateScheduleCommand;
using voicelife::test::Check;

namespace {

/** @brief 将测试 Unix 秒转换为日程时间。 @param seconds Unix 秒。 @return 日程时间。 */
DateTime At(int64_t seconds) { return DateTime{std::chrono::seconds{seconds}}; }

/** @brief 为 Repository 注入测试提供可控结果。 */
class FakeScheduleRepository final : public ScheduleRepository {
   public:
    /**
     * @brief 返回预设日程或读取错误。
     * @return 预设的读取结果。
     */
    Result<std::vector<Schedule>> FindAll() const override {
        ++find_all_calls;
        if (fail_find_all) return Result<std::vector<Schedule>>::Failure(ErrorCode::kUnavailable, "读取故障");
        return Result<std::vector<Schedule>>::Success(schedules);
    }

    /**
     * @brief 返回预设日程或读取错误。
     * @param query 查询条件。
     * @return 匹配的日程或读取错误。
     */
    Result<std::vector<Schedule>> Find(const QueryScheduleCommand& query) const override {
        ++find_calls;
        if (fail_find_all) return Result<std::vector<Schedule>>::Failure(ErrorCode::kUnavailable, "读取故障");
        std::vector<Schedule> matched;
        for (const Schedule& schedule : schedules) {
            if (query.schedule_id.has_value() && schedule.id != *query.schedule_id) continue;
            if (query.status != ScheduleStatusFilter::kAll &&
                schedule.status != static_cast<ScheduleStatus>(query.status))
                continue;
            if (query.start_from.has_value() &&
                (!schedule.start_time.has_value() || *schedule.start_time < *query.start_from))
                continue;
            if (query.start_to.has_value() &&
                (!schedule.start_time.has_value() || *schedule.start_time > *query.start_to))
                continue;
            matched.push_back(schedule);
        }
        std::sort(matched.begin(), matched.end(), [](const Schedule& left, const Schedule& right) {
            if (left.start_time != right.start_time) {
                if (!left.start_time.has_value()) return false;
                if (!right.start_time.has_value()) return true;
                return *left.start_time < *right.start_time;
            }
            return left.id < right.id;
        });
        const auto begin = std::min(static_cast<std::size_t>(query.offset), matched.size());
        const auto count = std::min(static_cast<std::size_t>(query.limit), matched.size() - begin);
        return Result<std::vector<Schedule>>::Success(
            std::vector<Schedule>(matched.begin() + static_cast<std::ptrdiff_t>(begin),
                                  matched.begin() + static_cast<std::ptrdiff_t>(begin + count)));
    }

    /**
     * @brief 统计匹配查询条件的日程数量。
     * @param query 查询条件。
     * @return 匹配数量或读取错误。
     */
    Result<int64_t> Count(const QueryScheduleCommand& query) const override {
        ++count_calls;
        if (fail_find_all) return Result<int64_t>::Failure(ErrorCode::kUnavailable, "读取故障");
        int64_t total = 0;
        for (const Schedule& schedule : schedules) {
            if (query.schedule_id.has_value() && schedule.id != *query.schedule_id) continue;
            if (query.status != ScheduleStatusFilter::kAll &&
                schedule.status != static_cast<ScheduleStatus>(query.status))
                continue;
            if (query.start_from.has_value() &&
                (!schedule.start_time.has_value() || *schedule.start_time < *query.start_from))
                continue;
            if (query.start_to.has_value() &&
                (!schedule.start_time.has_value() || *schedule.start_time > *query.start_to))
                continue;
            ++total;
        }
        return Result<int64_t>::Success(total);
    }

    /**
     * @brief 按 ID 读取日程或返回读取错误。
     * @param id 日程 ID。
     * @return 日程或错误。
     */
    Result<Schedule> FindById(ScheduleId id) const override {
        ++find_by_id_calls;
        if (fail_find_all) return Result<Schedule>::Failure(ErrorCode::kUnavailable, "读取故障");
        for (const Schedule& schedule : schedules) {
            if (schedule.id == id) return Result<Schedule>::Success(schedule);
        }
        return Result<Schedule>::Failure(ErrorCode::kNotFound, "未找到指定日程");
    }

    /**
     * @brief 返回可能重叠或临近的日程。
     * @param start 窗口开始时间。
     * @param end 窗口结束时间。
     * @param exclude_id 排除的日程 ID。
     * @return 匹配的日程或错误。
     */
    Result<std::vector<Schedule>> FindOverlapping(DateTime start, DateTime end,
                                                  std::optional<ScheduleId> exclude_id) const override {
        ++find_overlapping_calls;
        if (fail_find_all) return Result<std::vector<Schedule>>::Failure(ErrorCode::kUnavailable, "读取故障");
        std::vector<Schedule> matched;
        for (const Schedule& schedule : schedules) {
            if (schedule.status != ScheduleStatus::kActive || !schedule.start_time.has_value()) continue;
            if (exclude_id.has_value() && schedule.id == *exclude_id) continue;
            const DateTime schedule_start = *schedule.start_time;
            const DateTime schedule_end = schedule.end_time.value_or(schedule_start);
            if (schedule_start <= end && schedule_end >= start) matched.push_back(schedule);
        }
        std::sort(matched.begin(), matched.end(),
                  [](const Schedule& left, const Schedule& right) { return *left.start_time < *right.start_time; });
        return Result<std::vector<Schedule>>::Success(std::move(matched));
    }

    /**
     * @brief 保存日程或返回预设写入错误。
     * @param schedule 待保存日程。
     * @return 补齐标识后的日程或写入错误。
     */
    Result<Schedule> Insert(const Schedule& schedule) override {
        ++insert_calls;
        if (fail_insert) return Result<Schedule>::Failure(ErrorCode::kInternal, "写入故障");
        Schedule stored = schedule;
        stored.id = next_id;
        stored.created_at = At(1'900'000'000);
        stored.updated_at = stored.created_at;
        schedules.push_back(stored);
        return Result<Schedule>::Success(std::move(stored));
    }

    /**
     * @brief 更新预设日程或返回写入错误。
     * @param schedule 待更新日程。
     * @return 更新状态。
     */
    voicelife::Status Update(const Schedule& schedule) override {
        ++update_calls;
        if (fail_update) return voicelife::Status::Error(ErrorCode::kInternal, "更新故障");
        for (Schedule& existing : schedules) {
            if (existing.id == schedule.id) {
                existing = schedule;
                return voicelife::Status::Ok();
            }
        }
        return voicelife::Status::Error(ErrorCode::kNotFound, "日程不存在");
    }

    /**
     * @brief 将日程标记为已取消。
     * @param id 日程标识。
     * @return 更新状态。
     */
    voicelife::Status Delete(int64_t id) override {
        ++delete_calls;
        for (Schedule& existing : schedules) {
            if (existing.id != id) continue;
            if (existing.status == ScheduleStatus::kCancelled) {
                return voicelife::Status::Error(ErrorCode::kConflict, "日程已取消，不能重复删除");
            }
            existing.status = ScheduleStatus::kCancelled;
            return voicelife::Status::Ok();
        }
        return voicelife::Status::Error(ErrorCode::kNotFound, "未找到指定日程");
    }

    std::vector<Schedule> schedules;
    bool fail_find_all = false;
    bool fail_insert = false;
    bool fail_update = false;
    int64_t next_id = 9001;
    mutable int find_all_calls = 0;
    mutable int find_calls = 0;
    mutable int count_calls = 0;
    mutable int find_by_id_calls = 0;
    mutable int find_overlapping_calls = 0;
    int insert_calls = 0;
    int update_calls = 0;
    int delete_calls = 0;
};

/** @brief 为基础仓储注入测试提供独立的操作仓储替身。 */
class FakeScheduleOperationRepository final : public ScheduleOperationRepository {
   public:
    /**
     * @brief 保存操作记录并补齐标识和时间。
     * @param operation 待保存操作。
     * @return 保存后的操作记录。
     */
    Result<OperationRecord> InsertOperation(const OperationRecord& operation) override {
        OperationRecord stored = operation;
        stored.id = next_id++;
        stored.operated_at = DateTime{std::chrono::seconds{1'900'000'000}};
        operations.push_back(stored);
        return Result<OperationRecord>::Success(std::move(stored));
    }

    /**
     * @brief 返回预设的操作记录，忽略筛选与分页。
     * @param query 查询条件。
     * @return 操作记录集合。
     */
    Result<std::vector<OperationRecord>> FindOperations(const QueryOperationCommand& query) const override {
        (void)query;
        return Result<std::vector<OperationRecord>>::Success(operations);
    }

    /**
     * @brief 返回预设操作的总条数，忽略筛选。
     * @param query 查询条件。
     * @return 操作总数。
     */
    Result<int64_t> CountOperations(const QueryOperationCommand& query) const override {
        (void)query;
        return Result<int64_t>::Success(static_cast<int64_t>(operations.size()));
    }

    std::vector<OperationRecord> operations;
    OperationId next_id = 1;
};

/**
 * @brief 创建用于冲突和临近判断的日程。
 * @param id 日程标识。
 * @param start 开始时间 Unix 秒。
 * @param end 结束时间 Unix 秒。
 * @return 有效日程。
 */
Schedule ExistingSchedule(int64_t id, int64_t start, int64_t end) {
    return {
        .id = id,
        .event = "已有日程",
        .start_time = At(start),
        .end_time = At(end),
        .location = std::nullopt,
        .notes = std::nullopt,
        .rule_id = std::nullopt,
        .status = ScheduleStatus::kActive,
        .created_at = At(start - 100),
        .updated_at = At(start - 100),
    };
}

/**
 * @brief 验证创建和查询会保留 Repository 读取错误。
 * @return 无。
 */
void CheckFindAllFailure() {
    FakeScheduleRepository repository;
    FakeScheduleOperationRepository operation_repository;
    repository.fail_find_all = true;
    const ScheduleService service(repository);

    const auto created = service.create_schedule(CreateScheduleCommand{.event = "读取失败",
                                                                       .start_time = At(10),
                                                                       .end_time = At(20),
                                                                       .location = std::nullopt,
                                                                       .notes = std::nullopt});
    Check(created.result.status.code == ErrorCode::kUnavailable && created.result.status.message == "读取故障",
          "创建应返回 Repository 读取错误");
    Check(repository.insert_calls == 0, "读取失败后不应继续写入");

    const auto queried = service.query_schedule({});
    Check(queried.result.status.code == ErrorCode::kUnavailable && queried.result.status.message == "读取故障",
          "查询应返回 Repository 读取错误");
    Check(repository.find_overlapping_calls == 1 && repository.find_calls == 1,
          "有时间创建和查询应分别使用 Repository 能力");
}

/**
 * @brief 验证创建会保留 Repository 写入错误。
 * @return 无。
 */
void CheckInsertFailure() {
    FakeScheduleRepository repository;
    FakeScheduleOperationRepository operation_repository;
    repository.fail_insert = true;
    const ScheduleService service(repository);

    const auto result = service.create_schedule(CreateScheduleCommand{.event = "写入失败",
                                                                      .start_time = std::nullopt,
                                                                      .end_time = std::nullopt,
                                                                      .location = std::nullopt,
                                                                      .notes = std::nullopt});
    Check(result.result.status.code == ErrorCode::kInternal && result.result.status.message == "写入故障",
          "创建应返回 Repository 写入错误");
    Check(repository.insert_calls == 1, "写入失败前应完成一次写入");
}

/**
 * @brief 验证冲突拒绝、忽略冲突和临近日程仍由 Service 编排。
 * @return 无。
 */
void CheckConflictOrchestration() {
    FakeScheduleRepository repository;
    FakeScheduleOperationRepository operation_repository;
    repository.schedules.push_back(ExistingSchedule(1, 2'000, 3'000));
    ScheduleService service(repository);

    CreateScheduleCommand command{
        .event = "冲突日程",
        .start_time = At(2'500),
        .end_time = At(2'800),
        .location = std::nullopt,
        .notes = std::nullopt,
    };
    const auto rejected = service.create_schedule(command);
    Check(rejected.result.status.code == ErrorCode::kConflict && rejected.conflicts.size() == 1,
          "默认应拒绝 Repository 中的冲突日程");
    Check(repository.insert_calls == 0, "冲突拒绝分支不能写入");

    command.ignore_conflict = true;
    const auto ignored = service.create_schedule(command);
    Check(ignored.result.ok() && ignored.result.value && ignored.result.value->id == repository.next_id &&
              ignored.conflicts.size() == 1,
          "忽略冲突后应写入并保留冲突提示");
    Check(repository.insert_calls == 1, "忽略冲突应只写入一次");

    FakeScheduleRepository nearby_repository;
    FakeScheduleOperationRepository nearby_operation_repository;
    nearby_repository.schedules.push_back(ExistingSchedule(2, 4'000, 5'000));
    ScheduleService nearby_service(nearby_repository);
    const auto nearby = nearby_service.create_schedule(CreateScheduleCommand{
        .event = "临近日程",
        .start_time = At(3'500),
        .end_time = At(3'800),
        .location = std::nullopt,
        .notes = std::nullopt,
    });
    Check(
        nearby.result.ok() && nearby.nearby_schedules.size() == 1 && nearby.message == "日程创建成功，附近还有其他日程",
        "Repository 数据应参与 Service 临近判断");
}

/**
 * @brief 验证注入 Repository 后的筛选、排序和分页。
 * @return 无。
 */
void CheckRepositoryQuery() {
    FakeScheduleRepository repository;
    repository.schedules = {
        ExistingSchedule(3, 8'000, 8'100),
        ExistingSchedule(1, 6'000, 6'100),
        Schedule{
            .id = 2,
            .event = "无时间日程",
            .start_time = std::nullopt,
            .end_time = std::nullopt,
            .location = std::nullopt,
            .notes = std::nullopt,
            .rule_id = std::nullopt,
                .status = ScheduleStatus::kActive,
            .created_at = At(5'000),
            .updated_at = At(5'000),
        },
    };
    FakeScheduleOperationRepository operation_repository;
    const ScheduleService service(repository);
    QueryScheduleCommand command;
    command.status = ScheduleStatusFilter::kAll;
    command.limit = 2;
    command.offset = 1;

    const auto result = service.query_schedule(command);
    Check(result.result.ok() && result.total == 3 && result.result.value.size() == 2,
          "Repository 查询应在 Service 中应用分页");
    Check(result.result.value[0].id == 3 && result.result.value[1].id == 2,
          "Repository 查询应按时间排序并将无时间日程放在末尾");
}

/**
 * @brief 验证修改会读取并写回 Repository，且保留仓储错误。
 * @return 无。
 */
void CheckRepositoryUpdate() {
    FakeScheduleRepository repository;
    repository.schedules = {ExistingSchedule(7, 10'000, 11'000)};
    FakeScheduleOperationRepository operation_repository;
    ScheduleService service(repository);
    UpdateScheduleCommand command;
    command.schedule_id = 7;
    command.event = " 更新后的日程 ";

    const auto updated = service.update_schedule(command);
    Check(updated.result.ok() && updated.result.value.has_value() && updated.result.value->event == "更新后的日程",
          "修改应返回 Repository 保存后的日程");
    Check(repository.find_by_id_calls == 1 && repository.update_calls == 1 &&
              repository.schedules.front().event == "更新后的日程",
          "修改应读取并写回 Repository");

    repository.fail_update = true;
    command.event = "失败修改";
    const auto failed = service.update_schedule(command);
    Check(failed.result.status.code == ErrorCode::kInternal && failed.result.status.message == "更新故障" &&
              !failed.result.value.has_value(),
          "修改应保留 Repository 更新错误");
}

/**
 * @brief 验证删除会通过 Repository 软取消，并保留读取及更新错误。
 * @return 无。
 */
void CheckRepositoryDelete() {
    FakeScheduleRepository repository;
    repository.schedules = {ExistingSchedule(8, 12'000, 13'000)};
    FakeScheduleOperationRepository operation_repository;
    ScheduleService service(repository);

    const auto deleted = service.cancel_schedule(CancelScheduleCommand{.schedule_id = 8});
    Check(deleted.result.ok() && deleted.result.value && repository.delete_calls == 1 &&
              repository.schedules.front().status == ScheduleStatus::kCancelled,
          "删除应把 Repository 中的日程标记为已取消");

    const auto repeated = service.cancel_schedule(CancelScheduleCommand{.schedule_id = 8});
    Check(repeated.result.status.code == ErrorCode::kConflict && !repeated.result.value &&
              repeated.result.status.message == "日程已取消，不能重复删除",
          "重复删除已取消日程应返回冲突");

    const auto missing = service.cancel_schedule(CancelScheduleCommand{.schedule_id = 9});
    Check(missing.result.status.code == ErrorCode::kNotFound && !missing.result.value &&
              missing.result.status.message == "未找到指定日程",
          "删除不存在的日程应返回未找到");
}

}  // namespace

/** @brief 执行 ScheduleRepository 注入行为测试。 @return 全部断言通过时返回 0。 */
int main() {
    CheckFindAllFailure();
    CheckInsertFailure();
    CheckConflictOrchestration();
    CheckRepositoryQuery();
    CheckRepositoryUpdate();
    CheckRepositoryDelete();
    return 0;
}
