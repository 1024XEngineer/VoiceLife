#include "voicelife/im/im_gateway_adapter.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <type_traits>

#include "support/test_support.h"

using voicelife::ErrorCode;
using voicelife::Status;
using voicelife::test::Check;

namespace {

std::string ReadFixture(const char* name) {
    std::ifstream input(std::string(VOICELIFE_SOURCE_DIR) + "/contracts/im-gateway/v1/fixtures/" + name);
    Check(input.good(), "共享 IM fixture 必须存在");
    std::ostringstream content;
    content << input.rdbuf();
    std::string compact = content.str();
    compact.erase(std::remove_if(compact.begin(), compact.end(), [](unsigned char value) {
                      return std::isspace(value) != 0;
                  }),
                  compact.end());
    return compact;
}

bool HasStringField(const std::string& json, const std::string& key, const std::string& value) {
    return json.find("\"" + key + "\":\"" + value + "\"") != std::string::npos;
}

class RecordingTransport final : public voicelife::im::ImTransportPort {
   public:
    Status Send(const voicelife::im::ImGatewayRequest& request) override {
        ++calls;
        last_request = request;
        return result;
    }

    Status result = Status::Ok();
    std::optional<voicelife::im::ImGatewayRequest> last_request;
    int calls = 0;
};

}  // namespace

int main() {
    static_assert(std::is_same_v<decltype(voicelife::application::NotificationIntent::schedule_id), std::string>);

    const std::string strong_fixture = ReadFixture("notification-strong.json");
    const std::string weak_fixture = ReadFixture("notification-weak.json");
    const std::string schedule_fixture = ReadFixture("schedule-receipt.json");
    Check(HasStringField(strong_fixture, "schemaVersion", std::string(voicelife::im::kDeviceContractVersion)),
          "C++ 与 TypeScript 必须共享契约版本");
    Check(HasStringField(strong_fixture, "scheduleId", "schedule-fixture"),
          "跨端 ScheduleId 必须是字符串");
    Check(HasStringField(strong_fixture, "reminderType", "strong") &&
              HasStringField(weak_fixture, "reminderType", "weak"),
          "C++ 主机测试必须消费强弱提醒共享 fixture");
    Check(HasStringField(schedule_fixture, "operationType", "created"),
          "C++ 主机测试必须消费日程回执共享 fixture");

    RecordingTransport transport;
    voicelife::im::ImGatewayAdapter gateway(transport);
    voicelife::application::NotificationIntent intent{
        .event_id = "event-1",
        .correlation_id = "request-1",
        .kind = "schedule.created",
        .schedule_id = "schedule-1",
        .summary = "架构评审",
    };

    Check(gateway.Publish(intent).code == ErrorCode::kUnavailable, "未配置 IM Gateway 不能发送通知");
    Check(gateway.Configure("http://gateway.local", "secret").code == ErrorCode::kInvalidArgument,
          "携带凭据的 IM Gateway 必须使用 HTTPS");
    Check(gateway.Configure("https://gateway.local", "").code == ErrorCode::kInvalidArgument,
          "IM Gateway 必须携带凭据引用解析结果");
    Check(gateway.Configure("https://gateway.local///", "secret").ok(), "合法 HTTPS 配置应通过");
    Check(gateway.Publish(intent).ok(), "已配置 Gateway 应发送通知");
    Check(transport.calls == 1 && transport.last_request.has_value(), "Gateway 应调用传输 Port");
    Check(transport.last_request->url == "https://gateway.local/v1/im/notifications", "Gateway 应生成版本化通知端点");
    Check(transport.last_request->bearer_token == "secret", "Gateway 应把凭据限制在传输边界");

    Check(gateway.Configure("", "").ok() && !gateway.configured(), "空配置应显式禁用 IM Gateway");
    return 0;
}
