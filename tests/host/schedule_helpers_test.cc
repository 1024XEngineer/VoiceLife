#include "helpers/schedule_occurrence_helpers.h"
#include "helpers/schedule_query_helpers.h"
#include "helpers/schedule_rule_result_helpers.h"
#include "rules/schedule_time_rules.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "support/in_memory_schedule_repository.h"
#include "support/test_support.h"
#include "voicelife/schedule/schedule_commands.h"
#include "voicelife/schedule/schedule_rule_results.h"
#include "voicelife/schedule/schedule_types.h"

using voicelife::ErrorCode;
using voicelife::Status;
using voicelife::schedule::DateTime;
using voicelife::schedule::FailedQueryScheduleRulesResult;
using voicelife::schedule::FindConflictingSchedules;
using voicelife::schedule::FindMaterializedScheduleOccurrence;
using voicelife::schedule::FindNearbySchedules;
using voicelife::schedule::MatchesScheduleQuery;
using voicelife::schedule::QueryScheduleCommand;
using voicelife::schedule::Schedule;
using voicelife::schedule::ScheduleStatus;
using voicelife::schedule::ValidateQueryScheduleCommand;
using voicelife::test::Check;
using voicelife::test::InMemoryScheduleRepository;

namespace {

/** @brief 将测试 Unix 秒转换为日程时间。 @param seconds Unix 秒。 @return 日程时间。 */
DateTime At(int64_t seconds) { return DateTime{std::chrono::seconds{seconds}}; }

/** @brief 构造带起止时间的日程。 @param event 标题。 @param start 开始秒。 @param end 结束秒。 @return 日程。 */
Schedule TimedSchedule(const std::string& event, int64_t start, int64_t end) {
    Schedule schedule;
    schedule.event = event;
    schedule.start_time = At(start);
    schedule.end_time = At(end);
    return schedule;
}

/** @brief 仅实现纯虚方法、保留其余默认实现的仓储，用于覆盖默认不可用分支。 */
class MinimalScheduleRepository final : public voicelife::schedule::ScheduleRepository {
   public:
    voicelife::Result<Schedule> Insert(const Schedule&) override {
        return voicelife::Result<Schedule>::Failure(ErrorCode::kUnavailable, "不支持");
    }
    voicelife::Result<std::vector<Schedule>> FindAll() const override {
        return voicelife::Result<std::vector<Schedule>>::Success({});
    }
};

}  // namespace

int main() {
    // —— 按关联 schedule_id 读取已物化实例：命中与关联失效两种分支 ——
    InMemoryScheduleRepository repository;
    Schedule linked = TimedSchedule("已物化实例", 4'071'171'600, 4'071'175'200);
    const auto inserted = repository.Insert(linked);
    Check(inserted.ok(), "应插入已物化实例");

    const auto found = FindMaterializedScheduleOccurrence(repository, 0, At(0), inserted.value->id);
    Check(found.ok() && found.value->has_value() && found.value->value().id == inserted.value->id,
          "按关联 ID 应命中已物化实例");

    const auto missing = FindMaterializedScheduleOccurrence(repository, 0, At(0), int64_t{999999});
    Check(missing.ok() && !missing.value->has_value(), "关联 ID 失效时应回退为空值");

    // —— 冲突 / 临近筛选跳过无开始时间、非活跃与同标识日程 ——
    Schedule candidate = TimedSchedule("候选日程", 4'000'000'000, 4'000'003'600);
    Schedule no_start;
    no_start.event = "无开始时间";
    Schedule cancelled = TimedSchedule("已取消日程", 4'000'000'000, 4'000'003'600);
    cancelled.status = ScheduleStatus::kCancelled;
    Schedule same_id = candidate;
    same_id.event = "同标识日程";
    const std::vector<Schedule> pool{no_start, cancelled, same_id};
    Check(FindConflictingSchedules(candidate, pool, std::nullopt).empty(),
          "冲突筛选应跳过无开始时间、非活跃与同标识日程");
    Check(FindNearbySchedules(candidate, pool).empty(), "临近筛选应跳过无开始时间、非活跃与同标识日程");

    // —— 查询条件校验：规则 ID 必须大于 0 ——
    QueryScheduleCommand bad_rule;
    bad_rule.rule_id = int64_t{0};
    Check(ValidateQueryScheduleCommand(bad_rule).code == ErrorCode::kInvalidArgument, "规则 ID 必须大于 0");

    // —— 查询匹配：按规则 ID 筛选无规则 ID 或规则 ID 不一致的日程 ——
    QueryScheduleCommand rule_filter;
    rule_filter.rule_id = int64_t{7};
    Schedule without_rule = TimedSchedule("无规则日程", 4'000'000'000, 4'000'003'600);
    Check(!MatchesScheduleQuery(without_rule, rule_filter), "无规则 ID 的日程应不匹配规则筛选");
    Schedule other_rule = without_rule;
    other_rule.rule_id = int64_t{8};
    Check(!MatchesScheduleQuery(other_rule, rule_filter), "规则 ID 不一致的日程应不匹配规则筛选");

    // —— 查询规则失败结果构造 ——
    const auto failed = FailedQueryScheduleRulesResult(Status::Error(ErrorCode::kUnavailable, "仓储不可用"));
    Check(!failed.status.ok() && failed.rules.empty() && failed.total == 0 && failed.error == "仓储不可用",
          "查询失败结果应携带错误信息并返回空集合");

    // —— 覆盖仓储默认实现：未覆写的方法应返回不可用 ——
    MinimalScheduleRepository minimal;
    Check(minimal.FindById(1).status.code == ErrorCode::kUnavailable, "默认 FindById 应返回不可用");
    Check(minimal.Find({}).status.code == ErrorCode::kUnavailable, "默认 Find 应返回不可用");
    Check(minimal.Count({}).status.code == ErrorCode::kUnavailable, "默认 Count 应返回不可用");
    Check(minimal.FindOverlapping(At(0), At(0), std::nullopt).status.code == ErrorCode::kUnavailable,
          "默认 FindOverlapping 应返回不可用");
    return 0;
}
