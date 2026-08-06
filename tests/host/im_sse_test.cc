// #127 固件 SSE 动作流解码器：主机测试（TDD 先写）。
// 验收来源：Issue #127 —— 强提醒窗口内以 SSE 接收 ReminderActionCommand，
// 帧以空行分隔（id/event/data 字段），需处理跨多次读的分帧、CRLF 行尾与
// 心跳注释帧；载荷为动作命令契约 JSON。

#include "voicelife/im/im_sse.h"

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "support/test_support.h"
#include "voicelife/contracts/im/reminder_action_command.h"
#include "voicelife/contracts/json.h"

using voicelife::contracts::im::ParseReminderActionCommand;
using voicelife::contracts::im::ReminderActionCommand;
using voicelife::im::SseDecoder;
using voicelife::im::SseFrame;
using voicelife::test::Check;

namespace {

std::string ReadFixture(const char* name) {
    std::ifstream input(std::string(VOICELIFE_SOURCE_DIR) + "/contracts/im-gateway/v1/fixtures/" + name);
    Check(input.good(), "共享 IM fixture 必须存在");
    std::ostringstream content;
    content << input.rdbuf();
    return content.str();
}

void TestSingleFrame() {
    SseDecoder decoder;
    std::vector<SseFrame> frames;
    decoder.Feed("id: command-1\nevent: reminder.action\ndata: {\"commandId\":\"command-1\"}\n\n", frames);

    Check(frames.size() == 1, "完整单帧必须产出一个事件");
    Check(frames[0].id == "command-1", "事件 id 必须被解析");
    Check(frames[0].event == "reminder.action", "事件类型必须被解析");
    Check(frames[0].data.find("\"commandId\":\"command-1\"") != std::string::npos, "事件载荷必须被解析");
}

void TestTwoFramesInOneFeed() {
    SseDecoder decoder;
    std::vector<SseFrame> frames;
    decoder.Feed(
        "id: command-1\nevent: reminder.action\ndata: {}\n\n"
        "id: command-2\nevent: reminder.action\ndata: {}\n\n",
        frames);

    Check(frames.size() == 2, "一次喂入两帧必须产出两个事件");
    Check(frames[0].id == "command-1" && frames[1].id == "command-2", "事件 id 必须按序解析");
}

void TestFrameSplitAcrossFeeds() {
    SseDecoder decoder;
    std::vector<SseFrame> frames;
    decoder.Feed("id: command-1\nevent: rem", frames);
    Check(frames.empty(), "不完整帧不得产出事件");

    decoder.Feed("inder.action\ndata: {}\n\n", frames);
    Check(frames.size() == 1, "跨多次喂入的帧必须完整产出");
    Check(frames[0].event == "reminder.action", "跨帧字段必须被完整解析");
}

void TestHeartbeatCommentIgnored() {
    SseDecoder decoder;
    std::vector<SseFrame> frames;
    decoder.Feed(
        ": keepalive\n\n"
        "id: command-1\nevent: reminder.action\ndata: {}\n\n",
        frames);

    Check(frames.size() == 1, "心跳注释帧不得产出事件");
    Check(frames[0].id == "command-1", "注释帧不得干扰后续帧");
}

void TestCrlfLineEndings() {
    SseDecoder decoder;
    std::vector<SseFrame> frames;
    decoder.Feed("id: command-1\r\nevent: reminder.action\r\ndata: {}\r\n\r\n", frames);

    Check(frames.size() == 1, "CRLF 行尾必须被解析");
    Check(frames[0].id == "command-1" && frames[0].event == "reminder.action", "CRLF 字段必须被解析");
}

void TestDataFieldsJoinWithNewline() {
    SseDecoder decoder;
    std::vector<SseFrame> frames;
    decoder.Feed("event: reminder.action\ndata: {\"a\":1}\ndata: {\"b\":2}\n\n", frames);

    Check(frames.size() == 1 && frames[0].data == "{\"a\":1}\n{\"b\":2}", "多行 data 必须以换行连接");
}

void TestResetClearsPartialFrame() {
    SseDecoder decoder;
    std::vector<SseFrame> frames;
    decoder.Feed("id: command-1\nevent: rem", frames);
    Check(frames.empty(), "复位前残留不完整帧");
    decoder.Reset();
    decoder.Feed("id: command-1\nevent: reminder.action\ndata: {}\n\n", frames);
    Check(frames.size() == 1, "复位后必须丢弃残留并正常解析新帧");
}

void TestCommandDataRoundTripsThroughContract() {
    SseDecoder decoder;
    std::vector<SseFrame> frames;
    // 网关以 JSON.stringify 将命令序列化为单行 data；fixture 为美化格式，
    // 去除换行以贴合线上线形（JSON 忽略空白，紧凑化后仍合法）。
    const std::string fixture = ReadFixture("reminder-action-command.json");
    std::string payload;
    for (const char c : fixture) {
        if (c != '\n' && c != '\r') {
            payload.push_back(c);
        }
    }
    decoder.Feed("id: command-fixture\nevent: reminder.action\ndata: " + payload + "\n\n", frames);

    Check(frames.size() == 1, "命令帧必须产出事件");
    voicelife::JsonValue root;
    Check(voicelife::ParseJson(frames[0].data, root).ok(), "事件载荷必须是合法 JSON");
    ReminderActionCommand command;
    Check(ParseReminderActionCommand(root, command).ok(), "事件载荷必须通过动作命令契约校验");
    Check(command.commandId == "command-fixture" && command.operationId == "operation-fixture",
          "命令标识必须与载荷一致");
}

}  // namespace

int main() {
    TestSingleFrame();
    TestTwoFramesInOneFeed();
    TestFrameSplitAcrossFeeds();
    TestHeartbeatCommentIgnored();
    TestCrlfLineEndings();
    TestDataFieldsJoinWithNewline();
    TestResetClearsPartialFrame();
    TestCommandDataRoundTripsThroughContract();
    return 0;
}
