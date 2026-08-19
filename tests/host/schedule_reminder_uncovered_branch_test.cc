#define main ExistingScheduleReminderTestMain
#include "schedule_reminder_service_test.cc"
#undef main

namespace {

/** @brief 验证重复注册回调不会清除已经被新任务替换的标识。 @return 无。 */
void CheckLateDuplicateResultKeepsReplacement() {
    ScriptedFixture fixture({MakeSchedule(1, "替换任务", At(1'100))});
    Check(fixture.reminder.Start().ok(), "替换任务测试应启动服务");
    Check(fixture.timing.register_commands.size() == 1, "启动应注册一条提醒");

    Schedule replaced = *fixture.repository.FindById(1).value;
    const int64_t replacement_id = *replaced.reminder_task_id + 1;
    replaced.reminder_task_id = replacement_id;
    Check(fixture.repository.Update(replaced).ok(), "应模拟提醒任务已经被替换");

    const RegisterTaskCommand& command = fixture.timing.register_commands.front();
    command.on_result(RegisterTaskResult::kRegistered);
    command.on_result(RegisterTaskResult::kDuplicate);
    const auto stored = fixture.repository.FindById(1);
    Check(stored.ok() && stored.value->reminder_task_id == replacement_id, "迟到的重复结果不应清除新任务标识");
}

/** @brief 验证重复结果幂等处理以及重复后命令拒绝的清理分支。 @return 无。 */
void CheckRetryResultOrderingBranches() {
    ScriptedFixture duplicate_fixture({MakeSchedule(1, "重复重试", At(1'100), 31)});
    duplicate_fixture.rules.rules.push_back(DailyRule(31));
    duplicate_fixture.rules.fail_create_next_count = 1;
    Check(duplicate_fixture.reminder.Start().ok(), "重复重试测试应启动服务");
    const RegisterTaskCommand initial = duplicate_fixture.timing.register_commands.front();
    const auto initial_id = voicelife::timing::TaskId::Create(initial.task_id.Value());
    initial.callback(*initial_id, Trigger(1'100));
    const RegisterTaskCommand retry = duplicate_fixture.timing.register_commands.back();
    retry.on_result(RegisterTaskResult::kDuplicate);
    retry.on_result(RegisterTaskResult::kDuplicate);
    retry.on_result(RegisterTaskResult::kRegistered);
    duplicate_fixture.reminder.Stop();
    Check(duplicate_fixture.timing.cancel_calls == 0, "重复结果清理后不应再取消重试任务");

    ScriptedFixture rejected_fixture({MakeSchedule(2, "拒绝重试", At(1'100), 32)});
    rejected_fixture.rules.rules.push_back(DailyRule(32));
    rejected_fixture.rules.fail_create_next_count = 1;
    Check(rejected_fixture.reminder.Start().ok(), "拒绝重试测试应启动服务");
    const RegisterTaskCommand rejected_initial = rejected_fixture.timing.register_commands.front();
    const auto rejected_id = voicelife::timing::TaskId::Create(rejected_initial.task_id.Value());
    rejected_fixture.timing.report_register_result = true;
    rejected_fixture.timing.register_result = RegisterTaskResult::kDuplicate;
    rejected_fixture.timing.register_acceptance = CommandAcceptance::kUnavailable;
    rejected_initial.callback(*rejected_id, Trigger(1'100));
    rejected_fixture.reminder.Stop();
    Check(rejected_fixture.timing.cancel_calls == 0, "重复结果先清理后不应留下被拒绝的重试任务");
}

/** @brief 验证规则撤销保留首个实例错误并继续处理重试取消。 @return 无。 */
void CheckSuspendKeepsFirstCancellationFailure() {
    ScriptedFixture fixture({
        MakeSchedule(1, "已到期实例", At(1'100), 33),
        MakeSchedule(2, "未来实例", At(1'200), 33),
    });
    fixture.rules.rules.push_back(DailyRule(33));
    fixture.rules.fail_create_next_count = 1;
    Check(fixture.reminder.Start().ok(), "组合取消失败测试应启动服务");

    const RegisterTaskCommand due = fixture.timing.register_commands.front();
    const auto due_id = voicelife::timing::TaskId::Create(due.task_id.Value());
    due.callback(*due_id, Trigger(1'100));
    Check(fixture.timing.register_commands.size() == 3, "到期失败后应保留未来提醒并注册重试");

    fixture.timing.cancel_acceptance = CommandAcceptance::kUnavailable;
    const Status suspended = fixture.reminder.SuspendRuleReminders(33);
    Check(!suspended.ok(), "实例和重试取消均失败时应返回首个错误");
    Check(fixture.timing.cancel_calls == 2, "即使实例取消失败也应继续尝试取消生成重试");
}

}  // namespace

int main() {
    CheckLateDuplicateResultKeepsReplacement();
    CheckRetryResultOrderingBranches();
    CheckSuspendKeepsFirstCancellationFailure();
    return 0;
}
