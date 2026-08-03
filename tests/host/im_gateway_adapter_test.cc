#include "voicelife/im/im_gateway_adapter.h"

#include <optional>

#include "support/test_support.h"

using voicelife::ErrorCode;
using voicelife::Status;
using voicelife::test::Check;

namespace {

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
    Check(transport.last_request->url == "https://gateway.local/v1/notification-intents", "Gateway 应生成稳定语义端点");
    Check(transport.last_request->bearer_token == "secret", "Gateway 应把凭据限制在传输边界");

    Check(gateway.Configure("", "").ok() && !gateway.configured(), "空配置应显式禁用 IM Gateway");
    return 0;
}
