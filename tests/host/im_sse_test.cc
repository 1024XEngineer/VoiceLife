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
    Check(decoder.BufferedBytes() != 0, "不完整帧必须保留已读字节");

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

void TestCrlfSplitAcrossFeeds() {
    SseDecoder decoder;
    std::vector<SseFrame> frames;
    // \r 落在本次喂入末尾、\n 落在下次喂入开头时，不得把两处拼成虚假的空行边界。
    decoder.Feed("id: command-1\r", frames);
    Check(frames.empty(), "未完结行不得产出事件");
    decoder.Feed("\nevent: reminder.action\ndata: {}\n\n", frames);
    Check(frames.size() == 1, "CRLF 跨喂入切分必须解析为单个完整帧");
    Check(frames[0].id == "command-1" && frames[0].event == "reminder.action", "跨喂入 CRLF 字段必须完整解析");
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

void TestFeedBufferOverflowFlag() {
    SseDecoder decoder;
    std::vector<SseFrame> frames;
    // 单个未完成帧超过上限必须触发溢出标记，且不得产出事件。
    const std::string huge(SseDecoder::kMaxFrameBytes + 1, 'x');
    decoder.Feed(huge, frames);
    Check(decoder.Overflowed(), "超长未完成帧必须触发溢出标记");
    Check(frames.empty(), "溢出时不得产出事件");

    // 溢出后继续喂入不再累积（缓冲已被清空）；复位后标记清除且可正常解析。
    decoder.Feed(huge, frames);
    Check(decoder.Overflowed(), "溢出后必须保持溢出标记");
    decoder.Reset();
    Check(!decoder.Overflowed(), "复位必须清除溢出标记");
    decoder.Feed("id: command-1\nevent: reminder.action\ndata: {}\n\n", frames);
    Check(frames.size() == 1, "复位后必须能正常解析新帧");

    // 一次喂入多个合计超过上限的合法小帧不得误报溢出（以帧边界为准）。
    SseDecoder batch;
    std::vector<SseFrame> batch_frames;
    std::string payload;
    payload.reserve(SseDecoder::kMaxFrameBytes * 2);
    for (size_t i = 0; i < 400; ++i) {
        payload += "id: command-\n\n";
    }
    batch.Feed(payload, batch_frames);
    Check(!batch.Overflowed(), "多个合法小帧合计超限不得误报溢出");
    Check(batch_frames.size() == 400, "多个合法小帧必须全部产出");
}

}  // namespace

int main() {
    TestSingleFrame();
    TestTwoFramesInOneFeed();
    TestFrameSplitAcrossFeeds();
    TestHeartbeatCommentIgnored();
    TestCrlfLineEndings();
    TestCrlfSplitAcrossFeeds();
    TestDataFieldsJoinWithNewline();
    TestResetClearsPartialFrame();
    TestCommandDataRoundTripsThroughContract();
    TestFeedBufferOverflowFlag();
    return 0;
}
