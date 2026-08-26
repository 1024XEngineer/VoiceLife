// #127 动作通道主机测试共享夹具（TDD 先写）。
// 供 im_action_channel_test.cc 复用：假传输/凭据/执行器/时钟/动作流、
// 命令与窗口构造器、结果回传校验。保持在 500 行以内，超限应拆新夹具头。

#pragma once

#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "support/test_support.h"
#include "voicelife/contracts/im/reminder_action_command.h"
#include "voicelife/contracts/im/reminder_action_result.h"
#include "voicelife/contracts/json.h"
#include "voicelife/im/im_action_channel.h"
#include "voicelife/im/im_clock.h"
#include "voicelife/im/im_credentials.h"
#include "voicelife/im/im_transport.h"

namespace voicelife::test::action_channel {

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
using voicelife::im::StreamRead;
using voicelife::im::StreamReadStatus;

constexpr const char* kDeviceId = "device-fixture";
constexpr const char* kToken = "device-token";
constexpr const char* kNow = "2026-08-03T00:01:00.000Z";
constexpr const char* kWindowExpires = "2026-08-03T00:10:00.000Z";

/// 读取共享受理结果 fixture，保证强/弱窗口语义与网关 TS 契约一致。
std::string ReadFixture(const char* name) {
    std::ifstream input(std::string(VOICELIFE_SOURCE_DIR) + "/contracts/im-gateway/v1/fixtures/" + name);
    Check(input.good(), "共享 IM fixture 必须存在");
    std::ostringstream content;
    content << input.rdbuf();
    return content.str();
}

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
    ImHttpResponse Get(const ImHttpRequest& request) override { return Post(request); }
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

/// 可控当前时间的假时钟。sequence 非空时按调用次序逐次返回，耗尽后回退 now。
class FakeClock : public ImClock {
   public:
    std::string now = kNow;
    std::vector<std::string> sequence;
    size_t seq_idx = 0;

    std::string NowIso() override {
        if (seq_idx < sequence.size()) {
            return sequence[seq_idx++];
        }
        return now;
    }
};

/// 从预置命令队列拉取并记录 Open/Close 游标的假动作流。
class FakeStream : public ImActionCommandStream {
   public:
    std::vector<ReminderActionCommand> commands;
    std::vector<std::string> open_cursors;
    bool open_result = true;
    int next_calls = 0;
    int close_count = 0;
    /// 命令耗尽后的终结状态：默认正常结束，测试可改为 kNetworkError/kProtocolError。
    StreamReadStatus terminal = StreamReadStatus::kEndOfStream;

    bool Open(const std::string& last_event_id) override {
        open_cursors.push_back(last_event_id);
        return open_result;
    }
    StreamRead Next() override {
        ++next_calls;
        if (commands.empty()) {
            return {terminal, {}};
        }
        ReminderActionCommand command = commands.front();
        commands.erase(commands.begin());
        return {StreamReadStatus::kCommand, command};
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

}  // namespace voicelife::test::action_channel
