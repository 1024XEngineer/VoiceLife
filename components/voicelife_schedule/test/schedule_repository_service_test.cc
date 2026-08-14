#include <chrono>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "support/test_support.h"
#include "voicelife/schedule/schedule_repository.h"
#include "voicelife/schedule/schedule_service.h"

using voicelife::ErrorCode;
using voicelife::Result;
using voicelife::schedule::CreateScheduleCommand;
using voicelife::schedule::DateTime;
using voicelife::schedule::QueryScheduleCommand;
using voicelife::schedule::Schedule;
using voicelife::schedule::ScheduleRepository;
using voicelife::schedule::ScheduleService;
using voicelife::schedule::ScheduleStatus;
using voicelife::schedule::ScheduleStatusFilter;
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

    std::vector<Schedule> schedules;
    bool fail_find_all = false;
    bool fail_insert = false;
    int64_t next_id = 9001;
    mutable int find_all_calls = 0;
    int insert_calls = 0;
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
    repository.fail_find_all = true;
    const ScheduleService service(repository);

    const auto created = service.create_schedule(CreateScheduleCommand{.event = "读取失败",
                                                                       .start_time = std::nullopt,
                                                                       .end_time = std::nullopt,
                                                                       .location = std::nullopt,
                                                                       .notes = std::nullopt,
                                                                       .idempotency_key = std::nullopt});
    Check(created.status.code == ErrorCode::kUnavailable && created.error == "读取现有日程失败：读取故障",
          "创建应返回 Repository 读取错误");
    Check(repository.insert_calls == 0, "读取失败后不应继续写入");

    const auto queried = service.query_schedule({});
    Check(queried.status.code == ErrorCode::kUnavailable && queried.error == "读取故障",
          "查询应返回 Repository 读取错误");
    Check(repository.find_all_calls == 2, "创建和查询应分别调用一次 Repository");
}

/**
 * @brief 验证创建会保留 Repository 写入错误。
 * @return 无。
 */
void CheckInsertFailure() {
    FakeScheduleRepository repository;
    repository.fail_insert = true;
    const ScheduleService service(repository);

    const auto result = service.create_schedule(CreateScheduleCommand{.event = "写入失败",
                                                                      .start_time = std::nullopt,
                                                                      .end_time = std::nullopt,
                                                                      .location = std::nullopt,
                                                                      .notes = std::nullopt,
                                                                      .idempotency_key = std::nullopt});
    Check(result.status.code == ErrorCode::kInternal && result.error == "保存日程失败：写入故障",
          "创建应返回 Repository 写入错误");
    Check(repository.find_all_calls == 1 && repository.insert_calls == 1, "写入失败前应完成冲突读取和一次写入");
}

/**
 * @brief 验证冲突拒绝、忽略冲突和临近日程仍由 Service 编排。
 * @return 无。
 */
void CheckConflictOrchestration() {
    FakeScheduleRepository repository;
    repository.schedules.push_back(ExistingSchedule(1, 2'000, 3'000));
    ScheduleService service(repository);

    CreateScheduleCommand command{
        .event = "冲突日程",
        .start_time = At(2'500),
        .end_time = At(2'800),
        .location = std::nullopt,
        .notes = std::nullopt,
        .idempotency_key = std::nullopt,
    };
    const auto rejected = service.create_schedule(command);
    Check(rejected.status.code == ErrorCode::kConflict && rejected.conflicts.size() == 1,
          "默认应拒绝 Repository 中的冲突日程");
    Check(repository.insert_calls == 0, "冲突拒绝分支不能写入");

    command.ignore_conflict = true;
    const auto ignored = service.create_schedule(command);
    Check(ignored.status.ok() && ignored.schedule->id == repository.next_id && ignored.conflicts.size() == 1,
          "忽略冲突后应写入并保留冲突提示");
    Check(repository.insert_calls == 1, "忽略冲突应只写入一次");

    FakeScheduleRepository nearby_repository;
    nearby_repository.schedules.push_back(ExistingSchedule(2, 4'000, 5'000));
    ScheduleService nearby_service(nearby_repository);
    const auto nearby = nearby_service.create_schedule(CreateScheduleCommand{
        .event = "临近日程",
        .start_time = At(5'600),
        .end_time = At(6'000),
        .location = std::nullopt,
        .notes = std::nullopt,
        .idempotency_key = std::nullopt,
    });
    Check(
        nearby.status.ok() && nearby.nearby_schedules.size() == 1 && nearby.message == "日程创建成功，附近还有其他日程",
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
    const ScheduleService service(repository);
    QueryScheduleCommand command;
    command.status = ScheduleStatusFilter::kAll;
    command.limit = 2;
    command.offset = 1;

    const auto result = service.query_schedule(command);
    Check(result.status.ok() && result.total == 3 && result.schedules.size() == 2,
          "Repository 查询应在 Service 中应用分页");
    Check(result.schedules[0].id == 3 && result.schedules[1].id == 2,
          "Repository 查询应按时间排序并将无时间日程放在末尾");
}

/**
 * @brief 验证创建键的合法性校验在触碰 Repository 前完成。
 * @return 无。
 */
void CheckIdempotencyKeyValidation() {
    FakeScheduleRepository repository;
    const ScheduleService service(repository);

    const auto empty_key = service.create_schedule(CreateScheduleCommand{.event = "空创建键",
                                                                         .start_time = std::nullopt,
                                                                         .end_time = std::nullopt,
                                                                         .location = std::nullopt,
                                                                         .notes = std::nullopt,
                                                                         .idempotency_key = ""});
    Check(empty_key.status.code == ErrorCode::kInvalidArgument && empty_key.error == "日程创建键无效",
          "空创建键应被 Service 拒绝");

    const auto long_key = service.create_schedule(CreateScheduleCommand{.event = "超长创建键",
                                                                        .start_time = std::nullopt,
                                                                        .end_time = std::nullopt,
                                                                        .location = std::nullopt,
                                                                        .notes = std::nullopt,
                                                                        .idempotency_key = std::string(129, 'k')});
    Check(long_key.status.code == ErrorCode::kInvalidArgument && long_key.error == "日程创建键无效",
          "超过 128 字符的创建键应被 Service 拒绝");
    Check(repository.find_all_calls == 0 && repository.insert_calls == 0, "创建键校验失败不应触碰 Repository");
}

/**
 * @brief 验证 Repository 未实现幂等与到期提醒接口时错误被保留。
 * @return 无。
 */
void CheckRepositoryUnsupportedIdempotency() {
    // 未覆写幂等接口，走 ScheduleRepository 基类的默认实现。
    FakeScheduleRepository repository;
    const ScheduleService service(repository);
    const auto created = service.create_schedule(CreateScheduleCommand{.event = "无幂等支持",
                                                                       .start_time = std::nullopt,
                                                                       .end_time = std::nullopt,
                                                                       .location = std::nullopt,
                                                                       .notes = std::nullopt,
                                                                       .idempotency_key = "unsupported-key"});
    Check(created.status.code == ErrorCode::kUnavailable &&
              created.error == "读取已创建日程失败：当前仓储不支持查询日程创建键",
          "仓储不支持幂等查询时应保留底层错误");
    Check(repository.insert_calls == 0, "幂等查询失败后不应继续写入");

    // 覆写查询返回空结果，但保留基类默认的 InsertOnce 实现。
    class IdempotencyAwareRepository final : public ScheduleRepository {
       public:
        Result<std::vector<Schedule>> FindAll() const override {
            return Result<std::vector<Schedule>>::Success(schedules);
        }
        Result<Schedule> Insert(const Schedule& schedule) override {
            auto stored = schedule;
            stored.id = 1;
            schedules.push_back(stored);
            return Result<Schedule>::Success(std::move(stored));
        }
        Result<std::optional<Schedule>> FindByIdempotencyKey(std::string_view key) const override {
            (void)key;
            return Result<std::optional<Schedule>>::Success(std::nullopt);
        }
        std::vector<Schedule> schedules;
    };
    IdempotencyAwareRepository aware_repository;
    const ScheduleService aware_service(aware_repository);
    const auto stored = aware_service.create_schedule(CreateScheduleCommand{.event = "无幂等写入",
                                                                            .start_time = std::nullopt,
                                                                            .end_time = std::nullopt,
                                                                            .location = std::nullopt,
                                                                            .notes = std::nullopt,
                                                                            .idempotency_key = "unsupported-insert"});
    Check(stored.status.code == ErrorCode::kUnavailable && stored.error == "保存日程失败：当前仓储不支持幂等创建日程",
          "仓储不支持幂等写入时应保留底层错误");

    // 基类默认实现拒绝领取到期提醒。
    const auto due = repository.ClaimDueReminders(At(1'000), 4);
    Check(due.status.code == ErrorCode::kUnavailable && due.status.message == "当前仓储不支持领取到期提醒",
          "仓储默认实现应拒绝领取到期提醒");
}

}  // namespace

/** @brief 执行 ScheduleRepository 注入行为测试。 @return 全部断言通过时返回 0。 */
int main() {
    CheckFindAllFailure();
    CheckInsertFailure();
    CheckConflictOrchestration();
    CheckRepositoryQuery();
    CheckIdempotencyKeyValidation();
    CheckRepositoryUnsupportedIdempotency();
    return 0;
}
