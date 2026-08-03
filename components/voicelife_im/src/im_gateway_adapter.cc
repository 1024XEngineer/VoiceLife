#include "voicelife/im/im_gateway_adapter.h"

#include <utility>

namespace voicelife::im {

Status ImGatewayAdapter::Configure(std::string base_url, std::string bearer_token) {
    if (base_url.empty() && bearer_token.empty()) {
        base_url_.clear();
        bearer_token_.clear();
        return Status::Ok();
    }
    if (base_url.rfind("https://", 0) != 0) {
        return Status::Error(ErrorCode::kInvalidArgument, "携带设备凭据的 IM Gateway 必须使用 HTTPS");
    }
    if (bearer_token.empty()) {
        return Status::Error(ErrorCode::kInvalidArgument, "IM Gateway 缺少设备凭据");
    }
    while (!base_url.empty() && base_url.back() == '/') {
        base_url.pop_back();
    }
    base_url_ = std::move(base_url);
    bearer_token_ = std::move(bearer_token);
    return Status::Ok();
}

Status ImGatewayAdapter::Publish(const application::NotificationIntent& intent) {
    if (!configured()) {
        return Status::Error(ErrorCode::kUnavailable, "IM Gateway 未配置");
    }
    return transport_.Send({
        .url = base_url_ + "/v1/notification-intents",
        .bearer_token = bearer_token_,
        .intent = intent,
    });
}

}  // namespace voicelife::im
