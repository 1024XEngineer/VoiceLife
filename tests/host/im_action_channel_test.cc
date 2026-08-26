// #127 设备侧动作通道：主机测试（TDD 先写）。
// 验收来源：Issue #127 —— 命令执行、结果序列化与回传、operationId 幂等去重、
// 断线重连后相同 commandId 可重放但 operationId 只执行一次。
// 窗口/时间/游标分区语义见 im_action_channel_window_test.cc。

#include "voicelife/im/im_action_channel.h"

#include <limits>
#include <string>

#include "im_action_channel_test_support.h"

using voicelife::test::Check;

namespace {
using namespace voicelife::test::action_channel;

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

/// 构造覆盖 AppendJsonValue 全部值种类的嵌套 details。
voicelife::JsonValue MakeRichDetails() {
    return voicelife::JsonValue::Object({
        {"code", voicelife::JsonValue::String("E_CONFLICT")},
        {"count", voicelife::JsonValue::Number(3)},
        {"ratio", voicelife::JsonValue::Number(0.5)},
        {"ok", voicelife::JsonValue::Bool(true)},
        {"empty", voicelife::JsonValue()},
        {"tags", voicelife::JsonValue::Array({voicelife::JsonValue::String("a"), voicelife::JsonValue::String("b")})},
        {"nested", voicelife::JsonValue::Object({{"x", voicelife::JsonValue::Number(1)}})},
    });
}

void TestResultSerializationCarriesOptionalFields() {
    FakeTransport transport;
    FakeCredentials credentials;
    FakeExecutor executor;
    executor.result = MakeResult();
    executor.result.errorCode = "E_CONFLICT";
    executor.result.details = MakeRichDetails();
    FakeClock clock;
    ImReportingChannel reporting(transport, credentials);
    ImActionChannel channel(reporting, credentials, executor, clock);
    FakeStream stream;
    stream.commands.push_back(MakeCommand("command-1", "operation-1"));

    channel.Run(stream, MakeWindow());

    Check(transport.requests.size() == 1, "结果回传必须发生");
    voicelife::JsonValue root;
    Check(voicelife::ParseJson(transport.requests[0].body, root).ok(), "回传请求体必须是合法 JSON");
    const voicelife::JsonValue* error_code = root.Get("errorCode");
    Check(error_code != nullptr && error_code->IsString() && error_code->string == "E_CONFLICT",
          "errorCode 必须被序列化");
    const voicelife::JsonValue* details = root.Get("details");
    Check(details != nullptr && details->IsObject(), "details 对象必须被序列化");
    const voicelife::JsonValue* nested = details->Get("nested");
    Check(nested != nullptr && nested->IsObject() && nested->Get("x") != nullptr, "details 嵌套对象必须被序列化");
    const voicelife::JsonValue* count = details->Get("count");
    Check(count != nullptr && count->kind == voicelife::JsonValue::Kind::kNumber && count->number == 3,
          "details 整数必须被序列化");
    const voicelife::JsonValue* ratio = details->Get("ratio");
    Check(ratio != nullptr && ratio->kind == voicelife::JsonValue::Kind::kNumber && ratio->number == 0.5,
          "details 浮点必须被序列化");
    const voicelife::JsonValue* ok = details->Get("ok");
    Check(ok != nullptr && ok->kind == voicelife::JsonValue::Kind::kBool && ok->boolean, "details 布尔必须被序列化");
    const voicelife::JsonValue* none = details->Get("empty");
    Check(none != nullptr && none->kind == voicelife::JsonValue::Kind::kNull, "details null 必须被序列化");
    const voicelife::JsonValue* tags = details->Get("tags");
    Check(tags != nullptr && tags->IsArray() && tags->array.size() == 2, "details 数组必须被序列化");
}

void TestResultSerializationMinimal() {
    FakeTransport transport;
    FakeCredentials credentials;
    FakeExecutor executor;
    executor.result = MakeResult();
    executor.result.nextTriggerAt.reset();
    FakeClock clock;
    ImReportingChannel reporting(transport, credentials);
    ImActionChannel channel(reporting, credentials, executor, clock);
    FakeStream stream;
    stream.commands.push_back(MakeCommand("command-1", "operation-1"));

    channel.Run(stream, MakeWindow());

    Check(transport.requests.size() == 1, "结果回传必须发生");
    Check(transport.requests[0].body.find("\"nextTriggerAt\"") == std::string::npos, "缺省 nextTriggerAt 不得序列化");
    Check(transport.requests[0].body.find("\"errorCode\"") == std::string::npos, "缺省 errorCode 不得序列化");
    Check(transport.requests[0].body.find("\"details\"") == std::string::npos, "缺省 details 不得序列化");
}

void TestNonFiniteDetailsSerializedAsNull() {
    FakeTransport transport;
    FakeCredentials credentials;
    FakeExecutor executor;
    executor.result = MakeResult();
    executor.result.details =
        voicelife::JsonValue::Object({{"bad", voicelife::JsonValue::Number(std::numeric_limits<double>::quiet_NaN())}});
    FakeClock clock;
    ImReportingChannel reporting(transport, credentials);
    ImActionChannel channel(reporting, credentials, executor, clock);
    FakeStream stream;
    stream.commands.push_back(MakeCommand("command-1", "operation-1"));

    channel.Run(stream, MakeWindow());

    Check(transport.requests.size() == 1, "结果回传必须发生");
    Check(transport.requests[0].body.find("nan") == std::string::npos &&
              transport.requests[0].body.find("inf") == std::string::npos,
          "非有限数不得输出为 JSON 数字字面量");
    voicelife::JsonValue root;
    Check(voicelife::ParseJson(transport.requests[0].body, root).ok(), "含非有限数的回传体必须是合法 JSON");
}

void TestResultDetailsKeysEscaped() {
    FakeTransport transport;
    FakeCredentials credentials;
    FakeExecutor executor;
    executor.result = MakeResult();
    executor.result.details = voicelife::JsonValue::Object({
        {"quote\"key", voicelife::JsonValue::Number(1)},
        {"slash\\key", voicelife::JsonValue::Number(2)},
        {"new\nline", voicelife::JsonValue::Number(3)},
    });
    FakeClock clock;
    ImReportingChannel reporting(transport, credentials);
    ImActionChannel channel(reporting, credentials, executor, clock);
    FakeStream stream;
    stream.commands.push_back(MakeCommand("command-1", "operation-1"));

    channel.Run(stream, MakeWindow());

    Check(transport.requests.size() == 1, "含特殊键名的结果必须能回传，不得被本地契约校验拒绝");
    voicelife::JsonValue root;
    Check(voicelife::ParseJson(transport.requests[0].body, root).ok(), "含特殊键名的回传体必须是合法 JSON");
    const voicelife::JsonValue* details = root.Get("details");
    Check(details != nullptr && details->IsObject(), "details 对象必须可解析");
    Check(details->Get("quote\"key") != nullptr && details->Get("slash\\key") != nullptr &&
              details->Get("new\nline") != nullptr,
          "含引号/反斜杠/换行的键必须转义后原样往返");
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
    Check(first.next_calls == 1, "回执失败后应立即关闭当前流并交给重连逻辑");
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

void TestInvalidExecutorResultFaults() {
    FakeTransport transport;
    FakeCredentials credentials;
    FakeExecutor executor;
    executor.result = MakeResult();
    executor.result.status = "";  // 非法状态，序列化后无法通过契约解析
    FakeClock clock;
    ImReportingChannel reporting(transport, credentials);
    ImActionChannel channel(reporting, credentials, executor, clock);
    FakeStream stream;
    stream.commands.push_back(MakeCommand("command-1", "operation-1"));

    const ActionRunResult result = channel.Run(stream, MakeWindow());

    Check(result.status == ActionRunStatus::kDisconnected, "执行器返回非法结果必须故障隔离");
    Check(executor.calls.size() == 1, "非法结果命令应执行一次");
    Check(transport.requests.empty(), "非法结果不得回传");

    FakeStream replay;
    replay.commands.push_back(MakeCommand("command-1", "operation-1"));
    channel.Run(replay, MakeWindow());
    Check(executor.calls.size() == 2, "非法结果不得缓存，重放必须重新执行");
    Check(replay.open_cursors.size() == 1 && replay.open_cursors[0].empty(), "非法结果不得推进确认游标");
}

void TestCredentialRejectedDoesNotAdvanceCursor() {
    FakeTransport transport;
    FakeCredentials credentials;
    FakeExecutor executor;
    executor.result = MakeResult();
    FakeClock clock;
    ImReportingChannel reporting(transport, credentials);
    ImActionChannel channel(reporting, credentials, executor, clock);

    transport.next_status = ImTransportStatus::kCredentialRejected;
    transport.next_status_code = 401;
    FakeStream first;
    first.commands.push_back(MakeCommand("command-1", "operation-1"));
    const ActionRunResult first_result = channel.Run(first, MakeWindow());

    Check(first_result.status == ActionRunStatus::kDisconnected, "凭据被拒必须归类为可重连");
    Check(executor.calls.size() == 1, "凭据被拒时命令仍执行一次");

    transport.next_status = ImTransportStatus::kSuccess;
    FakeStream replay;
    replay.commands.push_back(MakeCommand("command-1", "operation-1"));
    channel.Run(replay, MakeWindow());
    Check(replay.open_cursors.size() == 1 && replay.open_cursors[0].empty(),
          "凭据被拒不得推进确认游标，网关应重放该命令");
}

void TestServerRejectionDoesNotAdvanceCursor() {
    FakeTransport transport;
    FakeCredentials credentials;
    FakeExecutor executor;
    executor.result = MakeResult();
    FakeClock clock;
    ImReportingChannel reporting(transport, credentials);
    ImActionChannel channel(reporting, credentials, executor, clock);

    // 网关对结果明确 4xx 拒绝：只有服务端受理才是业务 ACK，明确拒绝不得推进
    // 确认游标，网关需在重连后重放该命令。
    transport.next_status = ImTransportStatus::kHttpError;
    transport.next_status_code = 400;
    FakeStream first;
    first.commands.push_back(MakeCommand("command-1", "operation-1"));
    const ActionRunResult first_result = channel.Run(first, MakeWindow());

    Check(first_result.status == ActionRunStatus::kDisconnected, "网关拒绝结果必须归类为未确认可重连");
    Check(executor.calls.size() == 1, "命令应执行一次");
    Check(transport.requests.size() == 1, "结果应提交一次");

    transport.next_status = ImTransportStatus::kSuccess;
    FakeStream replay;
    replay.commands.push_back(MakeCommand("command-1", "operation-1"));
    channel.Run(replay, MakeWindow());
    Check(replay.open_cursors.size() == 1 && replay.open_cursors[0].empty(),
          "网关拒绝不得推进确认游标，重连应重放该命令");
}

void TestWrongDeviceIncrementsDropped() {
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

    Check(result.status == ActionRunStatus::kFinished, "本地丢弃后应正常结束");
    Check(result.dropped == 1, "错误 deviceId 命令必须计入 dropped");
    Check(executor.calls.empty(), "错误 deviceId 命令不得执行");
    Check(transport.requests.empty(), "错误 deviceId 命令不得回传");
}

}  // namespace

int main() {
    TestStrongWindowExecutesAndReports();
    TestResultSerializationCarriesOptionalFields();
    TestResultSerializationMinimal();
    TestNonFiniteDetailsSerializedAsNull();
    TestResultDetailsKeysEscaped();
    TestReconnectReplayExecutesOnce();
    TestExpiredCommandReportedAsExpired();
    TestWrongDeviceIdDroppedLocally();
    TestMismatchedTriggerDroppedLocally();
    TestDuplicateOperationIdWithinRunExecutesOnce();
    TestInvalidExecutorResultFaults();
    TestCredentialRejectedDoesNotAdvanceCursor();
    TestServerRejectionDoesNotAdvanceCursor();
    TestWrongDeviceIncrementsDropped();
    return 0;
}
