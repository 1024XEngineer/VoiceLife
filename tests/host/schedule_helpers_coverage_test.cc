#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "helpers/schedule_occurrence_helpers.h"
#include "helpers/schedule_query_helpers.h"
#include "support/test_support.h"
#include "voicelife/schedule/schedule_commands.h"
#include "voicelife/schedule/schedule_repository.h"
#include "voicelife/schedule/schedule_types.h"

using voicelife::ErrorCode;
using voicelife::schedule::DateTime;
using voicelife::schedule::FindMaterializedScheduleOccurrence;
using voicelife::schedule::MatchesScheduleQuery;
using voicelife::schedule::QueryScheduleCommand;
using voicelife::schedule::Schedule;
using voicelife::schedule::ScheduleRepository;
using voicelife::schedule::ScheduleStatus;
using voicelife::schedule::ScheduleStatusFilter;
using voicelife::schedule::ValidateQueryScheduleCommand;
using voicelife::test::Check;

namespace {

/** @brief 将测试 Unix 秒转换为日程时间。 @param seconds Unix 秒。 @return 日程时间。 */
DateTime At(int64_t seconds) { return DateTime{std::chrono::seconds{seconds}}; }

/** @brief 构造带起止时间的测试日程。 @param event 标题。 @param start 开始时间。 @param end 结束时间。 @return 日程。
 */
Schedule TimedSchedule(const std::string& event, int64_t start, int64_t end) {
    Schedule schedule;
    schedule.event = event;
    schedule.start_time = At(start);
    schedule.end_time = At(end);
    return schedule;
}

/** @brief 仅实现纯虚方法、用于覆盖物化实例读取失败分支的仓储。 */
class FailingFindScheduleRepository final : public ScheduleRepository {
   public:
    voicelife::Result<Schedule> Insert(const Schedule& schedule) override {
        return voicelife::Result<Schedule>::Success(schedule);
    }

    voicelife::Result<std::vector<Schedule>> FindAll() const override {
        return voicelife::Result<std::vector<Schedule>>::Success({});
    }

    [[nodiscard]] voicelife::Result<Schedule> FindById(voicelife::schedule::ScheduleId id) const override {
        (void)id;
        return voicelife::Result<Schedule>::Failure(ErrorCode::kUnavailable, "测试失败");
    }
};

}  // namespace

/**
 * @brief 执行新增的日程查询辅助覆盖测试。
 * @return 全部断言通过时返回 0。
 */
int main() {
    Schedule active = TimedSchedule("HouXuan Active Schedule", 4'000'000'000, 4'000'003'600);

    QueryScheduleCommand all_status;
    all_status.status = ScheduleStatusFilter::kAll;
    Check(MatchesScheduleQuery(active, all_status), "全状态筛选应匹配活跃日程");

    QueryScheduleCommand active_status;
    active_status.status = ScheduleStatusFilter::kActive;
    Check(MatchesScheduleQuery(active, active_status), "活跃状态筛选应匹配活跃日程");

    active.status = ScheduleStatus::kCompleted;
    Check(!MatchesScheduleQuery(active, active_status), "活跃状态筛选应拒绝完成日程");
    active.status = ScheduleStatus::kActive;

    QueryScheduleCommand keyword;
    keyword.keyword = "hOuXUAN";
    Check(MatchesScheduleQuery(active, keyword), "ASCII 关键词应按大小写归一化后匹配");

    keyword.keyword = "houxuan missing";
    Check(!MatchesScheduleQuery(active, keyword), "多关键词未全部命中时应拒绝");

    QueryScheduleCommand valid;
    valid.schedule_id = int64_t{1};
    valid.rule_id = int64_t{2};
    valid.start_from = At(4'000'000'000);
    valid.start_to = At(4'000'003'600);
    valid.limit = 50;
    valid.offset = 0;
    Check(ValidateQueryScheduleCommand(valid).ok(), "边界合法查询条件应通过校验");

    FailingFindScheduleRepository failing_repository;
    const auto failed = FindMaterializedScheduleOccurrence(failing_repository, 0, At(0), std::optional<int64_t>{1});
    Check(failed.status.code == ErrorCode::kUnavailable, "按关联 ID 读取失败应透传仓储错误");

    return 0;
}
