// #127 设备侧动作通道：主机测试（TDD 先写）。
// 验收来源：Issue #127 —— 强提醒窗口内建流、弱提醒不建流由调用方决定；
// 过期命令被拒绝；断线重连后相同 commandId 可重放但 operationId 只执行
// 一次；回传结果以 operationId 幂等确认，Last-Event-ID 不代替业务 ACK。

#include "voicelife/im/im_action_channel.h"

#include <string>
#include <vector>

#include "support/test_support.h"
#include "voicelife/contracts/im/reminder_action_command.h"
#include "voicelife/contracts/im/reminder_action_result.h"
#include "voicelife/contracts/json.h"
#include "voicelife/im/im_clock.h"
#include "voicelife/im/im_credentials.h"
#include "voicelife/im/im_transport.h"

using voicelife::contracts::im::ParseReminderActionCommand;
using voicelife::contracts::im::ParseReminderActionResult;
using voicelife::contracts::im::ReminderActionCommand;
using voicelife::contracts::im::ReminderActionResult;
using voicelife::im::ActionRunResult;
using voicelife::im::ActionRunStatus;
using voicelife::im::ActionWindow;
using voicelife::im::ImActionChannel;
using voicelife::im::ImActionCommandStream;
using voicelife::im::ImActionExecutor;
using voicelife::im::ImClock;
using voicelife::im::ImCredentialProvider;
using voicelife::im::ImHttpHeader;
using voicelife::im::ImHttpRequest;
using voicelife::im::ImHttpResponse;
using voicelife::im::ImReportingChannel;
using voicelife::im::ImTransport;
using voicelife::im::ImTransportStatus;
using voicelife::test::Check;

namespace {

constexpr const char* kDeviceId = "device-fixture";
constexpr const char* kToken = "device-token";
constexpr const char* kNow = "2026-08-03T00:01:00.000Z";
constexpr const char* kWindowExpires = "2026-08-03T00:10:00.000Z";

/// 记录请求并可控返回结果的假传输。
class FakeTransport : public ImTransport {
   public:
    std::vector<ImHttpRequest> requests;
    ImTransportStatus next_status = ImTransportStatus::kSuccess;
    int next_status_code = 200;

    ImHttpResponse Post(const ImHttpRequest& request) override {
        requests.push_back(request);
        ImHttpResponse response;
        response.status = next_status;
        response.status_code = next_status_code;
        response.message = "fake";
        return response;
    }
};

/// 可控凭据的假凭据提供者。
class FakeCredentials : public ImCredentialProvider {
   public:
    std::string token = kToken;
    std::string device_id = kDeviceId;

    std::string DeviceToken() const override { return token; }
    std::string DeviceId() const override { return device_id; }
};

/// 记录执行调用并返回可控结果的假执行器。
class FakeExecutor : public ImActionExecutor {
   public:
    std::vector<ReminderActionCommand> calls;
    ReminderActionResult result;

    ReminderActionResult Execute(const ReminderActionCommand& command) override {
        calls.push_back(command);
        return result;
    }
};

/// 可控当前时间的假时钟。
class FakeClock : public ImClock {
   public:
    std::string now = kNow;

    std::string NowIso() override { return now; }
};

/// 从预置命令队列拉取并记录 Open/Close 游标的假动作流。
class FakeStream : public ImActionCommandStream {
   public:
    std::vector<ReminderActionCommand> commands;
    std::vector<std::string> open_cursors;
    int close_count = 0;

    void Open(const std::string& last_event_id) override { open_cursors.push_back(last_event_id); }
    std::optional<ReminderActionCommand> Next() override {
        if (commands.empty()) {
            return std::nullopt;
        }
        ReminderActionCommand command = commands.front();
        commands.erase(commands.begin());
        return command;
    }
    void Close() override { close_count++; }
};

/// 构造窗口内的合法 snooze 命令，覆盖字段后得到变体。
ReminderActionCommand MakeCommand(const std::string& command_id, const std::string& operation_id) {
    ReminderActionCommand command;
    command.schemaVersion = "1";
    command.commandId = command_id;
    command.operationId = operation_id;
    command.correlationId = "correlation-fixture";
    command.deviceId = kDeviceId;
    command.actorBindingId = "binding-fixture";
    command.reminderTriggerId = "trigger-fixture";
    command.action = "snooze";
    command.minutes = 10;
    command.occurredAt = "2026-08-03T00:00:00.000Z";
    command.expiresAt = "2026-08-03T00:05:00.000Z";
    return command;
}

ReminderActionResult MakeResult() {
    ReminderActionResult result;
    result.schemaVersion = "1";
    result.operationId = "operation-1";
    result.reminderTriggerId = "trigger-fixture";
    result.status = "succeeded";
    result.nextTriggerAt = "2026-08-03T00:11:00.000Z";
    result.occurredAt = kNow;
    return result;
}

ActionWindow MakeWindow() {
    ActionWindow window;
    window.reminderTriggerId = "trigger-fixture";
    window.expiresAt = kWindowExpires;
    return window;
}

std::string HeaderValue(const ImHttpRequest& request, const std::string& name) {
    for (const ImHttpHeader& header : request.headers) {
        if (header.name == name) {
            return header.value;
        }
    }
    return "";
}

/// 校验回传请求体可解析为动作结果且字段一致。
void CheckResultRoundTrips(const ImHttpRequest& request, const ReminderActionResult& expected) {
    voicelife::JsonValue root;
    Check(voicelife::ParseJson(request.body, root).ok(), "回传请求体必须是合法 JSON");
    ReminderActionResult parsed;
    Check(ParseReminderActionResult(root, parsed).ok(), "回传请求体必须通过契约校验");
    Check(parsed.operationId == expected.operationId && parsed.reminderTriggerId == expected.reminderTriggerId &&
              parsed.status == expected.status && parsed.nextTriggerAt == expected.nextTriggerAt &&
              parsed.occurredAt == expected.occurredAt,
          "回传请求体必须与执行结果一致");
}

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

void TestStrongWindowExecutesAndReports() {
    FakeTransport transport;
    FakeCredentials credentials;
    FakeExecutor executor;
    executor.result = MakeResult();
    FakeClock clock;
    ImReportingChannel reporting(transport, credentials);
    ImActionChannel channel(reporting, credentials, executor, clock);
    FakeStream stream;
    stream.commands.push_back(MakeCommand("command-1", "operation-1"));

    const ActionRunResult result = channel.Run(stream, MakeWindow());

    Check(result.status == ActionRunStatus::kFinished, "成功路径应正常结束");
    Check(result.executed == 1 && result.confirmed == 1, "成功路径应执行并确认一条命令");
    Check(executor.calls.size() == 1 && executor.calls[0].commandId == "command-1", "执行器必须收到命令");
    Check(transport.requests.size() == 1, "成功路径应回传一次结果");
    const ImHttpRequest& request = transport.requests[0];
    Check(request.path == "/v1/devices/device-fixture/reminder-actions/command-1/result",
          "结果必须回传到 commandId 对应的 result 路径");
    Check(request.method == "POST", "回传必须使用 POST");
    Check(HeaderValue(request, "Authorization") == "Bearer " + std::string(kToken), "回传必须携带设备令牌");
    Check(HeaderValue(request, "Idempotency-Key") == "operation-1", "回传必须以 operationId 作为幂等键");
    ReminderActionResult expected = executor.result;
    expected.operationId = "operation-1";
    expected.reminderTriggerId = "trigger-fixture";
    CheckResultRoundTrips(request, expected);
}

void TestReconnectReplayExecutesOnce() {
    FakeTransport transport;
    FakeCredentials credentials;
    FakeExecutor executor;
    executor.result = MakeResult();
    FakeClock clock;
    ImReportingChannel reporting(transport, credentials);
    ImActionChannel channel(reporting, credentials, executor, clock);

    // 第一轮：网络失败，结果未确认，游标不得推进。
    transport.next_status = ImTransportStatus::kNetworkFailure;
    FakeStream first;
    first.commands.push_back(MakeCommand("command-1", "operation-1"));
    const ActionRunResult first_result = channel.Run(first, MakeWindow());
    Check(first_result.status == ActionRunStatus::kDisconnected, "网络失败应归类为可重连");
    Check(executor.calls.size() == 1, "网络失败时命令仍执行一次");
    Check(transport.requests.size() == 1, "网络失败时仍应尝试回传");
    Check(HeaderValue(transport.requests[0], "Idempotency-Key") == "operation-1",
          "首次回传必须以 operationId 为幂等键");

    // 第二轮：重连后网关重放相同 commandId，但 operationId 只执行一次。
    transport.next_status = ImTransportStatus::kSuccess;
    FakeStream second;
    second.commands.push_back(MakeCommand("command-1", "operation-1"));
    Check(second.open_cursors.empty(), "重连前游标记录为空");
    const ActionRunResult second_result = channel.Run(second, MakeWindow());
    Check(second_result.status == ActionRunStatus::kFinished, "重连重放后应正常结束");
    Check(executor.calls.size() == 1, "相同 operationId 重放不得重复执行");
    Check(transport.requests.size() == 2, "重放后应再次回传缓存结果");
    Check(HeaderValue(transport.requests[1], "Idempotency-Key") == "operation-1",
          "重放回传必须复用相同 operationId 幂等键");
    Check(transport.requests[1].body == transport.requests[0].body, "重放回传必须携带相同结果体");

    // 第三轮：已确认的游标应作为 Last-Event-ID 交给网关。
    FakeStream third;
    channel.Run(third, MakeWindow());
    Check(third.open_cursors.size() == 1 && third.open_cursors[0] == "command-1",
          "已确认命令的 commandId 必须作为 Last-Event-ID 游标");
}

void TestExpiredCommandReportedAsExpired() {
    FakeTransport transport;
    FakeCredentials credentials;
    FakeExecutor executor;
    executor.result = MakeResult();
    FakeClock clock;
    ImReportingChannel reporting(transport, credentials);
    ImActionChannel channel(reporting, credentials, executor, clock);

    ReminderActionCommand expired = MakeCommand("command-1", "operation-1");
    expired.expiresAt = "2026-08-03T00:00:30.000Z";  // 早于当前时间 00:01
    FakeStream stream;
    stream.commands.push_back(expired);

    const ActionRunResult result = channel.Run(stream, MakeWindow());

    Check(result.status == ActionRunStatus::kFinished, "过期命令拒绝后应正常结束");
    Check(executor.calls.empty(), "过期命令不得执行");
    Check(transport.requests.size() == 1, "过期命令必须回传 expired 终态");
    Check(HeaderValue(transport.requests[0], "Idempotency-Key") == "operation-1",
          "过期回传必须以 operationId 为幂等键");
    voicelife::JsonValue root;
    Check(voicelife::ParseJson(transport.requests[0].body, root).ok(), "过期回传体必须是合法 JSON");
    ReminderActionResult parsed;
    Check(ParseReminderActionResult(root, parsed).ok(), "过期回传体必须通过契约校验");
    Check(parsed.status == "expired" && parsed.operationId == "operation-1", "过期命令必须回传 expired 状态与操作标识");
}

void TestWrongDeviceIdDroppedLocally() {
    FakeTransport transport;
    FakeCredentials credentials;
    FakeExecutor executor;
    executor.result = MakeResult();
    FakeClock clock;
    ImReportingChannel reporting(transport, credentials);
    ImActionChannel channel(reporting, credentials, executor, clock);

    ReminderActionCommand foreign = MakeCommand("command-1", "operation-1");
    foreign.deviceId = "other-device";
    FakeStream stream;
    stream.commands.push_back(foreign);

    const ActionRunResult result = channel.Run(stream, MakeWindow());

    Check(result.status == ActionRunStatus::kFinished, "非本设备命令本地丢弃后应正常结束");
    Check(executor.calls.empty(), "非本设备命令不得执行");
    Check(transport.requests.empty(), "非本设备命令不得回传");

    FakeStream next;
    channel.Run(next, MakeWindow());
    Check(next.open_cursors.size() == 1 && next.open_cursors[0] == "command-1",
          "本地丢弃的命令必须推进游标避免无限重放");
}

void TestMismatchedTriggerDroppedLocally() {
    FakeTransport transport;
    FakeCredentials credentials;
    FakeExecutor executor;
    executor.result = MakeResult();
    FakeClock clock;
    ImReportingChannel reporting(transport, credentials);
    ImActionChannel channel(reporting, credentials, executor, clock);

    ReminderActionCommand foreign_trigger = MakeCommand("command-1", "operation-1");
    foreign_trigger.reminderTriggerId = "other-trigger";
    FakeStream stream;
    stream.commands.push_back(foreign_trigger);

    const ActionRunResult result = channel.Run(stream, MakeWindow());

    Check(result.status == ActionRunStatus::kFinished, "窗口外命令本地丢弃后应正常结束");
    Check(executor.calls.empty(), "窗口外命令不得执行");
    Check(transport.requests.empty(), "窗口外命令不得回传");
}

void TestDuplicateOperationIdWithinRunExecutesOnce() {
    FakeTransport transport;
    FakeCredentials credentials;
    FakeExecutor executor;
    executor.result = MakeResult();
    FakeClock clock;
    ImReportingChannel reporting(transport, credentials);
    ImActionChannel channel(reporting, credentials, executor, clock);

    FakeStream stream;
    stream.commands.push_back(MakeCommand("command-1", "operation-1"));
    stream.commands.push_back(MakeCommand("command-2", "operation-1"));

    const ActionRunResult result = channel.Run(stream, MakeWindow());

    Check(result.status == ActionRunStatus::kFinished, "同窗重复命令处理后应正常结束");
    Check(executor.calls.size() == 1, "同 operationId 重复命令只执行一次");
    Check(transport.requests.size() == 2, "每条命令都应回传结果");
    Check(HeaderValue(transport.requests[0], "Idempotency-Key") == "operation-1" &&
              HeaderValue(transport.requests[1], "Idempotency-Key") == "operation-1",
          "重复命令回传必须复用相同 operationId 幂等键");
}

}  // namespace

int main() {
    TestExpiredWindowDoesNotOpenStream();
    TestStrongWindowExecutesAndReports();
    TestReconnectReplayExecutesOnce();
    TestExpiredCommandReportedAsExpired();
    TestWrongDeviceIdDroppedLocally();
    TestMismatchedTriggerDroppedLocally();
    TestDuplicateOperationIdWithinRunExecutesOnce();
    return 0;
}
