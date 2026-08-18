#include "voicelife/im/im_runtime.h"

#include <algorithm>
#include <cctype>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "voicelife/im/im_endpoint.h"

namespace voicelife::im {
namespace {

constexpr std::string_view kReadinessProbePath = "/v1/im/pairing-sessions/voicelife-runtime-readiness-probe";

bool IsSafeCredential(const std::string& value, std::size_t maximum_size) {
    if (value.empty() || value.size() > maximum_size) return false;
    return std::all_of(value.begin(), value.end(),
                       [](unsigned char character) { return character > 0x20U && character < 0x7fU; });
}

void SecureClear(std::string& value) {
    std::fill(value.begin(), value.end(), '\0');
    value.clear();
}

}  // namespace

bool IsTrustedSystemTime(time_t now) { return now >= kTrustedEpochMinimum; }

ImRuntime::ImRuntime(ImConfigProvider& config, ImCredentialProvider& credentials, ImRuntimeReadinessPort& readiness,
                     ImTransportFactory transport_factory)
    : config_(config),
      credentials_(credentials),
      readiness_(readiness),
      transport_factory_(std::move(transport_factory)) {}

Status ImRuntime::Start() {
    if (state_ == ImRuntimeState::kReady || state_ == ImRuntimeState::kDisabled) return start_status_;
    if (transport_) {
        state_ = ImRuntimeState::kProbing;
        start_status_ = Status::Ok();
        return start_status_;
    }

    auto config = config_.Load();
    if (!config.ok() || !config.value.has_value()) {
        state_ = ImRuntimeState::kUnconfigured;
        start_status_ = config.status;
        return start_status_;
    }
    if (!config.value->enabled) {
        state_ = ImRuntimeState::kDisabled;
        start_status_ = Status::Ok();
        return start_status_;
    }
    if (!IsHttpsGatewayUrl(config.value->gateway_origin)) {
        state_ = ImRuntimeState::kUnconfigured;
        start_status_ = Status::Error(ErrorCode::kInvalidArgument, "IM Gateway origin 必须是安全的 HTTPS origin");
        return start_status_;
    }
    user_id_ = config.value->user_id;
    const std::string device_id = credentials_.DeviceId();
    std::string device_token = credentials_.DeviceToken();
    if (device_id.empty() || device_token.empty()) {
        SecureClear(device_token);
        state_ = ImRuntimeState::kUnconfigured;
        start_status_ = Status::Error(ErrorCode::kNotFound, "IM 设备凭据未配置");
        return start_status_;
    }
    if (!IsSafeCredential(device_id, 128) || !IsSafeCredential(device_token, 512)) {
        SecureClear(device_token);
        state_ = ImRuntimeState::kUnconfigured;
        start_status_ = Status::Error(ErrorCode::kInvalidArgument, "IM 设备凭据格式无效");
        return start_status_;
    }
    SecureClear(device_token);
    if (!readiness_.NetworkReady()) {
        state_ = ImRuntimeState::kDegraded;
        start_status_ = Status::Error(ErrorCode::kUnavailable, "IM Runtime 等待网络就绪");
        return start_status_;
    }
    if (!readiness_.SystemTimeReady()) {
        state_ = ImRuntimeState::kDegraded;
        start_status_ = Status::Error(ErrorCode::kUnavailable, "IM Runtime 等待可信系统时间");
        return start_status_;
    }
    if (!transport_factory_) {
        state_ = ImRuntimeState::kDegraded;
        start_status_ = Status::Error(ErrorCode::kUnavailable, "IM HTTPS Transport factory 未配置");
        return start_status_;
    }
    transport_ = transport_factory_(config.value->gateway_origin);
    if (!transport_) {
        state_ = ImRuntimeState::kDegraded;
        start_status_ = Status::Error(ErrorCode::kUnavailable, "IM HTTPS Transport 初始化失败");
        return start_status_;
    }
    state_ = ImRuntimeState::kProbing;
    start_status_ = Status::Ok();
    return start_status_;
}

ImHttpResponse ImRuntime::ProbeGateway() {
    if (!transport_) {
        state_ = ImRuntimeState::kDegraded;
        return {.status = ImTransportStatus::kInvalidConfig,
                .status_code = 0,
                .body = {},
                .message = "IM HTTPS Transport 尚未创建"};
    }

    std::string device_token = credentials_.DeviceToken();
    if (!IsSafeCredential(device_token, 512)) {
        SecureClear(device_token);
        state_ = ImRuntimeState::kUnconfigured;
        return {.status = ImTransportStatus::kInvalidConfig,
                .status_code = 0,
                .body = {},
                .message = "IM 设备凭据格式无效"};
    }

    ImHttpRequest request;
    request.path = std::string(kReadinessProbePath);
    request.method = "GET";
    request.headers.push_back({.name = "Authorization", .value = "Bearer " + device_token});
    SecureClear(device_token);
    ImHttpResponse response = transport_->Get(request);
    // esp_http_client_perform 会在部分非 2xx 路径消费短响应体，因此设备侧可能只保留
    // 已认证的 404 状态；主机/其他实现保留 body 时则必须精确匹配 Gateway 文本。
    const bool authenticated_not_found = response.status == ImTransportStatus::kHttpError &&
                                         response.status_code == 404 &&
                                         (response.body.empty() || response.body == "Not Found");
    if (response.status == ImTransportStatus::kSuccess || authenticated_not_found) {
        if (!reporting_) reporting_ = std::make_unique<ImReportingChannel>(*transport_, credentials_);
        if (!pairing_) pairing_ = std::make_unique<ImPairingClient>(*transport_, credentials_);
        state_ = ImRuntimeState::kReady;
        start_status_ = Status::Ok();
    } else {
        reporting_.reset();
        pairing_.reset();
        state_ = ImRuntimeState::kDegraded;
        start_status_ = Status::Error(ErrorCode::kUnavailable, "IM Gateway 认证探针失败");
    }
    return response;
}

}  // namespace voicelife::im
