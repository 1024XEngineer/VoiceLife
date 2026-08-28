#define main ExistingScheduleMcpToolsReminderTestMain
#include "schedule_mcp_tools_reminder_test.cc"
#undef main

namespace {

/** @brief 验证周期规则更新成功后提醒重新同步失败的返回路径。 @return 无。 */
void CheckRuleUpdateReminderResyncFailure() {
    ReminderToolFixture fixture;
    Check(fixture.reminder.Start().ok(), "规则重同步测试应启动服务");
    Check(voicelife::mcp::RegisterScheduleMcpTools(fixture.server, fixture.service, fixture.rule_service,
                                                   fixture.operation_service, &fixture.reminder)
              .ok(),
          "规则重同步测试应注册工具");

    const auto created = fixture.server.call({
        .request_id = "create-before-resync-failure",
        .name = "schedule.create_rule",
        .arguments = {{"event", std::string("待重同步规则")}, {"freq_type", std::string("daily")}, {"start_date", std::string("2099-01-01")}, {"start_time", std::string("09:00:00")}},
    });
    Check(created.status.ok() && OutputString(created, "status") == "success", "更新前应成功创建周期规则");

    fixture.timing.register_acceptance = CommandAcceptance::kUnavailable;
    const auto updated = fixture.server.call({
        .request_id = "update-rule-resync-failure",
        .name = "schedule.update_rule",
        .arguments = {{"rule_id", int64_t{fixture.rules.rules.back().id}}, {"event", std::string("重同步失败规则")}},
    });
    Check(updated.status.ok() && OutputString(updated, "status") == "failure" &&
              OutputString(updated, "message").find("提醒同步失败") != std::string::npos,
          "规则保存成功但提醒重同步失败时应返回失败");
}

}  // namespace

int main() {
    CheckRuleUpdateReminderResyncFailure();
    return 0;
}
