#include <chrono>

#include "support/in_memory_schedule_repository.h"
#include "support/test_support.h"
#include "voicelife/schedule/schedule_service.h"

using voicelife::ErrorCode;
using voicelife::schedule::CreateScheduleCommand;
using voicelife::schedule::DeleteScheduleCommand;
using voicelife::schedule::ScheduleService;
using voicelife::test::Check;
using voicelife::test::InMemoryScheduleRepository;

namespace {

/**
 * @brief 验证非法或不存在的日程 ID 会被拒绝。
 * @param service 被测日程服务。
 * @return 无返回值；断言失败时终止测试。
 */
void CheckInvalidScheduleId(ScheduleService& service) {
    const auto invalid = service.delete_schedule(DeleteScheduleCommand{.schedule_id = 0});
    Check(invalid.status.code == ErrorCode::kInvalidArgument && !invalid.deleted && !invalid.error.empty(),
          "非正数日程 ID 应返回参数错误");

    const auto missing = service.delete_schedule(DeleteScheduleCommand{.schedule_id = 9999});
    Check(missing.status.code == ErrorCode::kNotFound && missing.schedule_id == 9999 && !missing.deleted &&
              !missing.error.empty(),
          "不存在的日程应返回未找到错误");

    const auto recurring = service.delete_schedule(DeleteScheduleCommand{.schedule_id = 1003});
    Check(recurring.status.code == ErrorCode::kInvalidArgument && !recurring.deleted,
          "周期规则生成的实例应改用 skip_schedule_occurrence");
}

/**
 * @brief 验证删除采用取消状态，并且同一日程不能重复删除。
 * @param service 被测日程服务。
 * @return 无返回值；断言失败时终止测试。
 */
void CheckSoftDelete(ScheduleService& service) {
    const auto deleted = service.delete_schedule(DeleteScheduleCommand{.schedule_id = 1001});
    Check(deleted.status.ok() && deleted.schedule_id == 1001 && deleted.deleted && deleted.error.empty(),
          "有效日程应成功取消并返回原 ID");

    const auto repeated = service.delete_schedule(DeleteScheduleCommand{.schedule_id = 1001});
    Check(repeated.status.code == ErrorCode::kConflict && !repeated.deleted && !repeated.error.empty(),
          "已取消日程不能重复删除");
}

/**
 * @brief 验证已取消日程不再参与有效日程的冲突判断。
 * @param service 被测日程服务。
 * @return 无返回值；断言失败时终止测试。
 */
void CheckCancelledScheduleIsInactive(const ScheduleService& service) {
    CreateScheduleCommand command;
    command.event = "替代团队周会";
    command.start_time = voicelife::schedule::DateTime{std::chrono::seconds{1'800'000'600}};
    command.end_time = voicelife::schedule::DateTime{std::chrono::seconds{1'800'001'200}};

    const auto created = service.create_schedule(command);
    Check(created.status.ok() && created.conflicts.empty(), "已取消日程不应继续阻止同时间段的新日程");
}

}  // namespace

/**
 * @brief 执行日程删除服务测试。
 * @return 全部断言通过时返回 0。
 */
int main() {
    InMemoryScheduleRepository repository(InMemoryScheduleRepository::DefaultSchedules());
    ScheduleService service(repository, repository);
    CheckInvalidScheduleId(service);
    CheckSoftDelete(service);
    CheckCancelledScheduleIsInactive(service);
    return 0;
}
