#include <fstream>
#include <sstream>
#include <string>

#include "support/test_support.h"
#include "voicelife/contracts/im/notification_intent.h"
#include "voicelife/contracts/json.h"

using voicelife::ErrorCode;
using voicelife::JsonValue;
using voicelife::Status;
using voicelife::contracts::im::kDeviceContractVersion;
using voicelife::contracts::im::NotificationIntent;
using voicelife::contracts::im::ParseNotificationIntent;
using voicelife::test::Check;

namespace {

std::string ReadFixture(const char* name) {
    std::ifstream input(std::string(VOICELIFE_SOURCE_DIR) + "/contracts/im-gateway/v1/fixtures/" + name);
    Check(input.good(), "共享 IM fixture 必须存在");
    std::ostringstream content;
    content << input.rdbuf();
    return content.str();
}

Status ParseFixture(const char* name, NotificationIntent& out) {
    JsonValue root;
    if (Status json_status = voicelife::ParseJson(ReadFixture(name), root); !json_status.ok()) {
        return json_status;
    }
    return ParseNotificationIntent(root, out);
}

void RequireRejected(const char* name, const char* message) {
    NotificationIntent intent;
    const Status status = ParseFixture(name, intent);
    Check(!status.ok() && status.code == ErrorCode::kInvalidArgument, message);
}

}  // namespace

int main() {
    // 强提醒：双端共享版本，字段与 TS 语义一致
    NotificationIntent strong;
    Check(ParseFixture("notification-strong.json", strong).ok(), "共享强提醒 fixture 必须被 C++ 解析");
    Check(strong.schemaVersion == kDeviceContractVersion, "C++ 与 TypeScript 必须共享设备契约版本");
    Check(strong.reminderType == "strong", "强提醒 fixture 的 reminderType 必须为 strong");
    Check(strong.actions.size() == 2 && strong.actions[0].type == "acknowledge" && strong.actions[1].type == "snooze" &&
              strong.actions[1].minutes == 10,
          "强提醒动作必须与 TS 语义一致");
    Check(strong.actions[0].label == "知道了" && strong.actions[1].label == "推迟 10 分钟",
          "UTF-8 动作标签必须被原样保留");
    Check(strong.content.title == "Fixture reminder", "通知内容标题必须被保留");
    Check(strong.recipient.deviceId == "device-fixture" && strong.recipient.userId == "user-fixture",
          "收件人字段必须被保留");
    Check(strong.reminderTriggerId == "trigger-fixture", "reminderTriggerId 必须被保留");

    // 弱提醒：不得携带动作
    NotificationIntent weak;
    Check(ParseFixture("notification-weak.json", weak).ok(), "共享弱提醒 fixture 必须被 C++ 解析");
    Check(weak.reminderType == "weak" && weak.actions.empty(), "弱提醒 fixture 不得携带动作");

    // 非法 fixture：与 TS 一致的拒绝语义
    RequireRejected("notification-invalid-version.json", "非法版本 fixture 必须被 C++ 拒绝");
    RequireRejected("notification-invalid-enum.json", "非法枚举 fixture 必须被 C++ 拒绝");
    RequireRejected("notification-invalid-time.json", "非法时间 fixture 必须被 C++ 拒绝");
    RequireRejected("notification-missing-field.json", "缺字段 fixture 必须被 C++ 拒绝");
    return 0;
}
