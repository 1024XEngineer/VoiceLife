#include "voicelife/im/im_config_store.h"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>

namespace voicelife::im {
namespace {

Result<ImRuntimeConfig> MissingRequired(std::string_view key, const Status& status) {
    const ErrorCode code = status.code == ErrorCode::kNotFound ? ErrorCode::kNotFound : ErrorCode::kUnavailable;
    return Result<ImRuntimeConfig>::Failure(code, "IM 安全配置字段不可用: " + std::string(key));
}

}  // namespace

StoredImConfigProvider::~StoredImConfigProvider() { ClearCredentials(); }

Result<ImRuntimeConfig> StoredImConfigProvider::Load() {
    ClearCredentials();
    if (!enabled_) {
        return Result<ImRuntimeConfig>::Success({.enabled = false, .gateway_origin = {}, .user_id = std::nullopt});
    }

    auto gateway_origin = store_.Read(kImGatewayOriginKey);
    if (!gateway_origin.ok() || !gateway_origin.value.has_value() || gateway_origin.value->empty()) {
        return MissingRequired(kImGatewayOriginKey, gateway_origin.status);
    }
    auto device_id = store_.Read(kImDeviceIdKey);
    if (!device_id.ok() || !device_id.value.has_value() || device_id.value->empty()) {
        return MissingRequired(kImDeviceIdKey, device_id.status);
    }
    auto device_token = store_.Read(kImDeviceTokenKey);
    if (!device_token.ok() || !device_token.value.has_value() || device_token.value->empty()) {
        return MissingRequired(kImDeviceTokenKey, device_token.status);
    }

    std::optional<std::string> user_id;
    auto stored_user_id = store_.Read(kImUserIdKey);
    if (stored_user_id.ok() && stored_user_id.value.has_value() && !stored_user_id.value->empty()) {
        user_id = std::move(*stored_user_id.value);
    } else if (stored_user_id.status.code != ErrorCode::kNotFound) {
        std::fill(device_token.value->begin(), device_token.value->end(), '\0');
        return Result<ImRuntimeConfig>::Failure(ErrorCode::kUnavailable, "读取 IM 用户引用失败");
    }

    device_id_ = std::move(*device_id.value);
    device_token_ = std::move(*device_token.value);
    return Result<ImRuntimeConfig>::Success({
        .enabled = true,
        .gateway_origin = std::move(*gateway_origin.value),
        .user_id = std::move(user_id),
    });
}

void StoredImConfigProvider::ClearCredentials() {
    std::fill(device_token_.begin(), device_token_.end(), '\0');
    device_token_.clear();
    device_id_.clear();
}

}  // namespace voicelife::im
