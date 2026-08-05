#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <string>

#include "support/test_support.h"

using voicelife::test::Check;

namespace {

constexpr const char* kDeviceContractVersion = "1";

std::string ReadFixture(const char* name) {
    std::ifstream input(std::string(VOICELIFE_SOURCE_DIR) + "/contracts/im-gateway/v1/fixtures/" + name);
    Check(input.good(), "共享 IM fixture 必须存在");
    std::ostringstream content;
    content << input.rdbuf();
    std::string compact = content.str();
    compact.erase(
        std::remove_if(compact.begin(), compact.end(), [](unsigned char value) { return std::isspace(value) != 0; }),
        compact.end());
    return compact;
}

bool HasStringField(const std::string& json, const std::string& key, const std::string& value) {
    return json.find("\"" + key + "\":\"" + value + "\"") != std::string::npos;
}

}  // namespace

int main() {
    const std::string strong = ReadFixture("notification-strong.json");
    const std::string replay = ReadFixture("notification-strong-replay.json");
    const std::string weak = ReadFixture("notification-weak.json");
    const std::string conflict = ReadFixture("notification-conflict.json");
    const std::string schedule = ReadFixture("schedule-receipt.json");
    const std::string invalid_version = ReadFixture("notification-invalid-version.json");
    const std::string invalid_enum = ReadFixture("notification-invalid-enum.json");
    const std::string invalid_time = ReadFixture("notification-invalid-time.json");
    const std::string missing_field = ReadFixture("notification-missing-field.json");

    Check(HasStringField(strong, "schemaVersion", kDeviceContractVersion), "C++ 与 TypeScript 必须共享设备契约版本");
    Check(HasStringField(strong, "scheduleId", "schedule-fixture") &&
              HasStringField(schedule, "scheduleId", "schedule-fixture"),
          "Issue #65 规定跨端 ScheduleId 为不透明字符串");
    Check(HasStringField(strong, "reminderType", "strong") && strong.find("\"actions\":[{") != std::string::npos,
          "强提醒 fixture 必须携带动作");
    Check(HasStringField(weak, "reminderType", "weak") && weak.find("\"actions\":[]") != std::string::npos,
          "弱提醒 fixture 不得携带动作");
    Check(strong == replay, "完全相同的通知重放必须共享同一 wire contract");
    Check(HasStringField(conflict, "businessEventId", "event-fixture") && conflict != strong,
          "冲突 fixture 必须复用事件 ID 但改变内容");
    Check(HasStringField(schedule, "operationType", "created"), "日程回执 fixture 必须声明操作类型");

    Check(HasStringField(invalid_version, "schemaVersion", "999"), "非法版本 fixture 必须偏离当前版本");
    Check(HasStringField(invalid_enum, "reminderType", "urgent"), "非法枚举 fixture 必须包含未知提醒类型");
    Check(HasStringField(invalid_time, "plannedAt", "not-a-time"), "非法时间 fixture 必须包含错误时间");
    Check(missing_field.find("\"actions\":") == std::string::npos, "缺字段 fixture 必须省略 actions");
    return 0;
}
