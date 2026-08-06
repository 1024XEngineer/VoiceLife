// #127 设备侧动作通道：窗口与时间语义主机测试（TDD 先写）。
// 验收来源：Issue #127 —— 过期窗口/恰好截止不建流、运行中过期中止、
// 时区偏移命令有效期、连接失败可重连、跨窗口幂等与游标分区、
// 受理结果提取动作窗口（强有/弱无/畸形无）。

#include <optional>
#include <string>

#include "im_action_channel_test_support.h"
#include "voicelife/im/im_action_channel.h"

using voicelife::test::Check;

namespace {
using namespace voicelife::test::action_channel;

void TestExpiredWindowDoesNotOpenStream() {
    FakeTransport transport;
    FakeCredentials credentials;
    FakeExecutor executor;
    executor.result = MakeResult();
    FakeClock clock;
    clock.now = "2026-08-03T00:11:00.000Z";  // 窗口 00:10 已过
    ImReportingChannel reporting(transport, credentials);
    ImActionChannel channel(reporting, credentials, executor, clock);
    FakeStream stream;
    stream.commands.push_back(MakeCommand("command-1", "operation-1"));

    const ActionRunResult result = channel.Run(stream, MakeWindow());

    Check(result.status == ActionRunStatus::kWindowExpired, "过期窗口不得建立动作流");
    Check(stream.open_cursors.empty(), "过期窗口不得打开流连接");
    Check(executor.calls.empty(), "过期窗口不得执行命令");
    Check(transport.requests.empty(), "过期窗口不得回传结果");
}

void TestWindowBoundaryEqualIsExpired() {
    FakeTransport transport;
    FakeCredentials credentials;
    FakeExecutor executor;
    executor.result = MakeResult();
    FakeClock clock;
    clock.now = kWindowExpires;  // 恰为窗口截止时刻
    ImReportingChannel reporting(transport, credentials);
    ImActionChannel channel(reporting, credentials, executor, clock);
    FakeStream stream;
    stream.commands.push_back(MakeCommand("command-1", "operation-1"));

    const ActionRunResult result = channel.Run(stream, MakeWindow());

    Check(result.status == ActionRunStatus::kWindowExpired, "窗口恰在截止时刻必须视为过期");
    Check(stream.open_cursors.empty(), "过期窗口不得打开流连接");
    Check(executor.calls.empty(), "过期窗口不得执行命令");
    Check(transport.requests.empty(), "过期窗口不得回传结果");
}

void TestMidRunWindowExpiryClosesStream() {
    FakeTransport transport;
    FakeCredentials credentials;
    FakeExecutor executor;
    executor.result = MakeResult();
    FakeClock clock;
    // NowIso 调用次序：入口窗口检查、循环顶窗口检查、命令 1 有效期检查、
    // 循环顶窗口检查（此处置为过期）。
    clock.sequence = {"2026-08-03T00:01:00.000Z", "2026-08-03T00:01:00.000Z", "2026-08-03T00:01:00.000Z",
                      "2026-08-03T00:11:00.000Z"};
    ImReportingChannel reporting(transport, credentials);
    ImActionChannel channel(reporting, credentials, executor, clock);
    FakeStream stream;
    stream.commands.push_back(MakeCommand("command-1", "operation-1"));
    stream.commands.push_back(MakeCommand("command-2", "operation-2"));

    const ActionRunResult result = channel.Run(stream, MakeWindow());

    Check(result.status == ActionRunStatus::kWindowExpired, "窗口运行中过期必须中止动作通道");
    Check(executor.calls.size() == 1 && executor.calls[0].commandId == "command-1", "窗口过期后不得再执行命令");
    Check(transport.requests.size() == 1, "窗口过期后不得再回传结果");
}

void TestTimezoneOffsetCommandExpiry() {
    FakeTransport transport;
    FakeCredentials credentials;
    FakeExecutor executor;
    executor.result = MakeResult();
    FakeClock clock;
    clock.now = "2026-08-03T00:01:00.000Z";  // 00:01 UTC
    ImReportingChannel reporting(transport, credentials);
    ImActionChannel channel(reporting, credentials, executor, clock);
    ReminderActionCommand command = MakeCommand("command-1", "operation-1");
    command.expiresAt = "2026-08-03T08:00:00+08:00";  // 实际 00:00 UTC，早于当前时间
    FakeStream stream;
    stream.commands.push_back(command);

    const ActionRunResult result = channel.Run(stream, MakeWindow());

    Check(result.status == ActionRunStatus::kFinished, "过期命令拒绝后应正常结束");
    Check(executor.calls.empty(), "带时区偏移且已过期的命令不得执行");
    Check(transport.requests.size() == 1, "过期命令必须回传 expired 终态");
    voicelife::JsonValue root;
    Check(voicelife::ParseJson(transport.requests[0].body, root).ok(), "过期回传体必须是合法 JSON");
    ReminderActionResult parsed;
    Check(ParseReminderActionResult(root, parsed).ok(), "过期回传体必须通过契约校验");
    Check(parsed.status == "expired", "带时区偏移的过期命令必须回传 expired");
}

void TestOpenFailureReturnsDisconnected() {
    FakeTransport transport;
    FakeCredentials credentials;
    FakeExecutor executor;
    executor.result = MakeResult();
    FakeClock clock;
    ImReportingChannel reporting(transport, credentials);
    ImActionChannel channel(reporting, credentials, executor, clock);
    FakeStream stream;
    stream.open_result = false;

    const ActionRunResult result = channel.Run(stream, MakeWindow());

    Check(result.status == ActionRunStatus::kDisconnected, "连接建立失败必须归类为可重连");
    Check(executor.calls.empty(), "连接失败不得执行命令");
    Check(transport.requests.empty(), "连接失败不得回传结果");
}

void TestCrossWindowIdempotencyIsolation() {
    FakeTransport transport;
    FakeCredentials credentials;
    FakeExecutor executor;
    executor.result = MakeResult();
    FakeClock clock;
    ImReportingChannel reporting(transport, credentials);
    ImActionChannel channel(reporting, credentials, executor, clock);

    FakeStream first;
    first.commands.push_back(MakeCommand("command-1", "operation-1"));
    const ActionRunResult first_result = channel.Run(first, MakeWindow());
    Check(first_result.status == ActionRunStatus::kFinished && executor.calls.size() == 1, "窗口 A 中 op-1 应执行一次");

    ActionWindow window_b = MakeWindow();
    window_b.reminderTriggerId = "trigger-b";
    ReminderActionCommand command_b = MakeCommand("command-2", "operation-1");
    command_b.reminderTriggerId = "trigger-b";
    FakeStream second;
    second.commands.push_back(command_b);
    const ActionRunResult second_result = channel.Run(second, window_b);
    Check(second_result.status == ActionRunStatus::kFinished, "窗口 B 应正常结束");
    Check(executor.calls.size() == 2 && executor.calls[1].commandId == "command-2",
          "不同提醒触发的相同 operationId 不得重放缓存");
}

void TestCrossWindowCursorIsolation() {
    FakeTransport transport;
    FakeCredentials credentials;
    FakeExecutor executor;
    executor.result = MakeResult();
    FakeClock clock;
    ImReportingChannel reporting(transport, credentials);
    ImActionChannel channel(reporting, credentials, executor, clock);

    FakeStream first;
    first.commands.push_back(MakeCommand("command-1", "operation-1"));
    channel.Run(first, MakeWindow());

    ActionWindow window_b = MakeWindow();
    window_b.reminderTriggerId = "trigger-b";
    ReminderActionCommand command_b = MakeCommand("command-2", "operation-2");
    command_b.reminderTriggerId = "trigger-b";
    FakeStream second;
    second.commands.push_back(command_b);
    channel.Run(second, window_b);
    Check(second.open_cursors.size() == 1 && second.open_cursors[0].empty(),
          "窗口 B 的 Last-Event-ID 不得继承窗口 A 的游标");
}

void TestStrongSubmissionBodyYieldsActionWindow() {
    const std::optional<ActionWindow> window =
        voicelife::im::ExtractActionWindow(ReadFixture("notification-submission.json"));

    Check(window.has_value(), "强提醒受理结果必须提取出动作窗口");
    Check(window->reminderTriggerId == "trigger-fixture" && window->expiresAt == kWindowExpires,
          "动作窗口必须与强提醒受理结果的 actionStream 一致");
}

void TestWeakSubmissionBodyHasNoWindow() {
    const std::optional<ActionWindow> window =
        voicelife::im::ExtractActionWindow(ReadFixture("notification-submission-weak.json"));

    Check(!window.has_value(), "弱提醒受理结果不得提取出动作窗口");
}

void TestMalformedSubmissionBodyHasNoWindow() {
    const std::optional<ActionWindow> window = voicelife::im::ExtractActionWindow("{not-json");

    Check(!window.has_value(), "畸形受理结果不得提取出动作窗口");
}

void TestExecutedCachePrunedAfterWindowExpiry() {
    FakeTransport transport;
    FakeCredentials credentials;
    FakeExecutor executor;
    executor.result = MakeResult();
    FakeClock clock;
    ImReportingChannel reporting(transport, credentials);
    ImActionChannel channel(reporting, credentials, executor, clock);

    FakeStream first_stream;
    first_stream.commands.push_back(MakeCommand("command-1", "operation-1"));
    channel.Run(first_stream, MakeWindow());
    Check(executor.calls.size() == 1, "首窗 op-1 应执行一次并缓存");

    // 同一 reminderTriggerId 的新窗口，但旧窗口已过期：过期缓存必须清理，
    // op-1 视为新动作重新执行，而非复用旧窗口结果。
    clock.now = "2026-08-03T00:12:00.000Z";
    ActionWindow second = MakeWindow();
    second.expiresAt = "2026-08-03T00:20:00.000Z";
    ReminderActionCommand second_command = MakeCommand("command-2", "operation-1");
    second_command.expiresAt = "2026-08-03T00:18:00.000Z";
    FakeStream second_stream;
    second_stream.commands.push_back(second_command);
    const ActionRunResult second_result = channel.Run(second_stream, second);

    Check(second_result.status == ActionRunStatus::kFinished, "新窗口应正常结束");
    Check(executor.calls.size() == 2, "旧窗口过期缓存必须被清理，op-1 重新执行");
    Check(transport.requests.size() == 2, "新旧窗口都应回传结果");
}

}  // namespace

int main() {
    TestExpiredWindowDoesNotOpenStream();
    TestWindowBoundaryEqualIsExpired();
    TestMidRunWindowExpiryClosesStream();
    TestTimezoneOffsetCommandExpiry();
    TestOpenFailureReturnsDisconnected();
    TestCrossWindowIdempotencyIsolation();
    TestCrossWindowCursorIsolation();
    TestStrongSubmissionBodyYieldsActionWindow();
    TestWeakSubmissionBodyHasNoWindow();
    TestMalformedSubmissionBodyHasNoWindow();
    TestExecutedCachePrunedAfterWindowExpiry();
    return 0;
}
