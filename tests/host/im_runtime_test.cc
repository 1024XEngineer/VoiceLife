#include "voicelife/im/im_runtime.h"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "support/test_support.h"
#include "voicelife/im/im_config_store.h"
#include "voicelife/im/im_endpoint.h"
#include "voicelife/im/im_provisioning.h"
#include "voicelife/im/im_retry_policy.h"

namespace {

using voicelife::ErrorCode;
using voicelife::Result;
using voicelife::Status;
using voicelife::im::ImConfigProvider;
using voicelife::im::ImCredentialProvider;
using voicelife::im::ImHttpRequest;
using voicelife::im::ImHttpResponse;
using voicelife::im::ImRetryPolicy;
using voicelife::im::ImRuntime;
using voicelife::im::ImRuntimeConfig;
using voicelife::im::ImRuntimeReadinessPort;
using voicelife::im::ImRuntimeState;
using voicelife::im::ImSecretStorePort;
using voicelife::im::ImTransport;
using voicelife::im::StoredImConfigProvider;
using voicelife::test::Check;

class FakeConfig final : public ImConfigProvider {
   public:
    Result<ImRuntimeConfig> result = Result<ImRuntimeConfig>::Success(
        {.enabled = true, .gateway_origin = "https://gateway.example", .user_id = "user-test"});

    Result<ImRuntimeConfig> Load() override { return result; }
};

class FakeCredentials final : public ImCredentialProvider {
   public:
    std::string token = "device-token";
    std::string device_id = "device-test";

    std::string DeviceToken() const override { return token; }
    std::string DeviceId() const override { return device_id; }
};

class FakeReadiness final : public ImRuntimeReadinessPort {
   public:
    bool network_ready = true;
    bool system_time_ready = true;

    bool NetworkReady() const override { return network_ready; }
    bool SystemTimeReady() const override { return system_time_ready; }
};

class FakeTransport final : public ImTransport {
   public:
    ImHttpResponse Post(const ImHttpRequest&) override { return {}; }
};

class FakeSecretStore final : public ImSecretStorePort {
   public:
    Result<std::string> Read(std::string_view key) override {
        ++reads;
        if (fail_key.has_value() && key == *fail_key) {
            return Result<std::string>::Failure(ErrorCode::kUnavailable, "read failure");
        }
        if (fail_with == ErrorCode::kNotFound) return Result<std::string>::Failure(ErrorCode::kNotFound, "missing");
        if (fail_with == ErrorCode::kUnavailable) {
            return Result<std::string>::Failure(ErrorCode::kUnavailable, "read failure");
        }
        const auto found = values.find(std::string(key));
        if (found == values.end()) return Result<std::string>::Failure(ErrorCode::kNotFound, "missing");
        return Result<std::string>::Success(found->second);
    }

    int reads = 0;
    ErrorCode fail_with = ErrorCode::kNone;
    std::optional<std::string> fail_key;
    std::unordered_map<std::string, std::string> values;
};

struct RuntimeFixture {
    FakeConfig config;
    FakeCredentials credentials;
    FakeReadiness readiness;
    int factory_calls = 0;
    ImRuntime runtime{config, credentials, readiness, [this](const std::string& origin) {
                          ++factory_calls;
                          last_origin = origin;
                          return std::make_unique<FakeTransport>();
                      }};
    std::string last_origin;
};

void TestDisabledDoesNotConstructTransport() {
    RuntimeFixture fixture;
    fixture.config.result =
        Result<ImRuntimeConfig>::Success({.enabled = false, .gateway_origin = {}, .user_id = std::nullopt});

    Check(fixture.runtime.Start().ok(), "disabled IM 应安全启动");
    Check(fixture.runtime.state() == ImRuntimeState::kDisabled, "disabled 配置必须保持禁用状态");
    Check(fixture.factory_calls == 0 && fixture.runtime.reporting_channel() == nullptr,
          "disabled IM 不得构造 Transport 或上报通道");
}

void TestMissingConfigurationFailsClosed() {
    RuntimeFixture fixture;
    fixture.config.result = Result<ImRuntimeConfig>::Failure(ErrorCode::kNotFound, "missing");

    const Status status = fixture.runtime.Start();
    Check(!status.ok() && status.code == ErrorCode::kNotFound, "缺失配置必须返回稳定失败");
    Check(fixture.runtime.state() == ImRuntimeState::kUnconfigured, "缺失配置必须进入 unconfigured");
    Check(fixture.factory_calls == 0, "缺失配置不得尝试联网");
}

void TestInvalidConfigurationAndCredentialsFailClosed() {
    RuntimeFixture invalid_origin;
    invalid_origin.config.result = Result<ImRuntimeConfig>::Success(
        {.enabled = true, .gateway_origin = "http://gateway.example", .user_id = "user-test"});
    Check(invalid_origin.runtime.Start().code == ErrorCode::kInvalidArgument, "非 HTTPS origin 必须本地拒绝");
    Check(invalid_origin.runtime.state() == ImRuntimeState::kUnconfigured && invalid_origin.factory_calls == 0,
          "非法 origin 不得构造 Transport");

    RuntimeFixture missing_token;
    missing_token.credentials.token.clear();
    Check(missing_token.runtime.Start().code == ErrorCode::kNotFound, "缺 Token 必须 fail closed");
    Check(missing_token.runtime.state() == ImRuntimeState::kUnconfigured && missing_token.factory_calls == 0,
          "缺 Token 不得尝试联网");

    RuntimeFixture missing_device;
    missing_device.credentials.device_id.clear();
    Check(missing_device.runtime.Start().code == ErrorCode::kNotFound, "缺 deviceId 必须 fail closed");

    RuntimeFixture unsafe_token;
    unsafe_token.credentials.token = "unsafe\ncredential";
    Check(unsafe_token.runtime.Start().code == ErrorCode::kInvalidArgument, "含控制字符 Token 必须 fail closed");
    Check(unsafe_token.factory_calls == 0, "非法 Token 不得进入 HTTP header");

    RuntimeFixture oversized_token;
    oversized_token.credentials.token.assign(513, 'x');
    Check(oversized_token.runtime.Start().code == ErrorCode::kInvalidArgument, "超长 Token 必须 fail closed");
}

void TestGatewayOriginValidationRejectsAmbiguousUrls() {
    using voicelife::im::IsHttpsGatewayUrl;

    Check(IsHttpsGatewayUrl("https://gateway.example"), "标准 HTTPS origin 必须有效");
    Check(IsHttpsGatewayUrl("https://gateway.example:8443"), "带端口 HTTPS origin 必须有效");
    Check(IsHttpsGatewayUrl("https://[2001:db8::1]:8443"), "带端口 IPv6 HTTPS origin 必须有效");
    Check(!IsHttpsGatewayUrl("https://"), "空 authority 必须拒绝");
    Check(!IsHttpsGatewayUrl("https:///v1/im"), "缺 host 的 URL 必须拒绝");
    Check(!IsHttpsGatewayUrl("https://gateway.example/v1/im"), "origin 不得包含 path");
    Check(!IsHttpsGatewayUrl("https://gateway.example/"), "origin 不得包含尾部 path 分隔符");
    Check(!IsHttpsGatewayUrl("https://token@gateway.example"), "origin 不得包含 userinfo");
    Check(!IsHttpsGatewayUrl("https://gateway.example?x=1"), "origin 不得包含 query");
    Check(!IsHttpsGatewayUrl("https://gateway.example#x"), "origin 不得包含 fragment");
    Check(!IsHttpsGatewayUrl("https://gateway.example\n"), "origin 不得包含控制字符");
    Check(!IsHttpsGatewayUrl("https://gateway.example:abc"), "origin 端口必须为数字");
    Check(!IsHttpsGatewayUrl("https://gateway.example:70000"), "origin 端口必须在有效范围");
    Check(!IsHttpsGatewayUrl("https://gateway.example:443:444"), "非 IPv6 authority 不得含多个冒号");
    Check(!IsHttpsGatewayUrl("https://gateway_example"), "origin host 不得含非法 DNS 字符");

    // IPv6 与端口边界分支：补齐 helper 的分支覆盖。
    Check(IsHttpsGatewayUrl("https://[2001:db8::1]"), "IPv6 无端口使用默认 443 必须有效");
    Check(!IsHttpsGatewayUrl("https://[2001:db8::1]:"), "IPv6 空端口必须拒绝");
    Check(!IsHttpsGatewayUrl("https://[2001:db8::1]:0"), "IPv6 零端口必须拒绝");
    Check(!IsHttpsGatewayUrl("https://[2001:db8::1]:65536"), "IPv6 越界端口必须拒绝");
    Check(!IsHttpsGatewayUrl("https://[2001:db8::zz]"), "IPv6 非法十六进制必须拒绝");
    Check(!IsHttpsGatewayUrl("https://[::1]x"), "IPv6 后跟非法后缀必须拒绝");
    Check(IsHttpsGatewayUrl("https://[::1]"), "IPv6 无端口使用默认 443 必须有效");
    Check(!IsHttpsGatewayUrl("https://gateway.example:"), "空端口段必须拒绝");
    Check(!IsHttpsGatewayUrl("https://gateway.example:0"), "零端口必须拒绝");
    Check(!IsHttpsGatewayUrl("https://gateway.example:65536"), "越界端口必须拒绝");
    Check(!IsHttpsGatewayUrl("https://-gateway.example"), "host 首字符非字母数字必须拒绝");
    Check(!IsHttpsGatewayUrl("https://gateway.example-"), "host 尾字符非字母数字必须拒绝");
    Check(!IsHttpsGatewayUrl("https://gateway..example"), "host 空 label 必须拒绝");
}

void TestRetryPolicyIsBoundedAndStopsOnCredentials() {
    ImRetryPolicy policy({.maximum_attempts = 3, .initial_delay_ms = 1000, .maximum_delay_ms = 2500});
    ImHttpResponse network_failure{
        .status = voicelife::im::ImTransportStatus::kNetworkFailure, .body = {}, .message = {}};

    Check(policy.NextDelay(network_failure) == 1000, "第一次网络失败必须使用初始退避");
    Check(policy.NextDelay(network_failure) == 2000, "第二次网络失败必须指数退避");
    Check(policy.NextDelay(network_failure) == 2500, "退避必须受最大值约束");
    Check(!policy.NextDelay(network_failure).has_value(), "超过有限次数后必须停止重试");

    policy.Reset();
    ImHttpResponse throttled{
        .status = voicelife::im::ImTransportStatus::kHttpError, .status_code = 429, .body = {}, .message = {}};
    Check(policy.NextDelay(throttled) == 1000, "429 必须有限退避");
    ImHttpResponse unavailable{
        .status = voicelife::im::ImTransportStatus::kHttpError, .status_code = 503, .body = {}, .message = {}};
    Check(policy.NextDelay(unavailable) == 2000, "5xx 必须有限退避");

    policy.Reset();
    ImHttpResponse rejected{
        .status = voicelife::im::ImTransportStatus::kCredentialRejected, .status_code = 401, .body = {}, .message = {}};
    Check(!policy.NextDelay(rejected).has_value(), "401/403 不得紧密重试");
    Check(policy.attempts() == 0, "凭据拒绝不得消耗或推进退避计数");
    ImHttpResponse client_error{
        .status = voicelife::im::ImTransportStatus::kHttpError, .status_code = 400, .body = {}, .message = {}};
    Check(!policy.NextDelay(client_error).has_value(), "非限流 4xx 不得重试");
    ImHttpResponse invalid{.status = voicelife::im::ImTransportStatus::kInvalidConfig, .body = {}, .message = {}};
    Check(!policy.NextDelay(invalid).has_value(), "本地非法配置不得重试");
}

void TestRetryPolicyClampsAndHandles408() {
    // 构造钳制：零初始延迟提升到 1ms；maximum < initial 时钳到 initial。
    ImRetryPolicy clamped({.maximum_attempts = 2, .initial_delay_ms = 0, .maximum_delay_ms = 0});
    ImHttpResponse network_failure{
        .status = voicelife::im::ImTransportStatus::kNetworkFailure, .body = {}, .message = {}};
    Check(clamped.NextDelay(network_failure) == 1, "零初始延迟必须钳制为 1ms");
    Check(clamped.NextDelay(network_failure) == 1, "最大延迟低于初始延迟必须钳制到初始值");

    // 408 Request Timeout 属于可重试 HTTP 错误。
    ImRetryPolicy timeout_policy;
    ImHttpResponse request_timeout{
        .status = voicelife::im::ImTransportStatus::kHttpError, .status_code = 408, .body = {}, .message = {}};
    Check(timeout_policy.NextDelay(request_timeout).has_value(), "408 必须允许有限重试");
    Check(timeout_policy.attempts() == 1, "408 必须推进退避计数");
    timeout_policy.Reset();
    Check(timeout_policy.attempts() == 0, "Reset 必须清零退避计数");
}

void AppendProvisionField(std::vector<uint8_t>& request, std::string_view value) {
    request.push_back(static_cast<uint8_t>((value.size() >> 8U) & 0xffU));
    request.push_back(static_cast<uint8_t>(value.size() & 0xffU));
}

void TestProvisioningFrameIsStrictAndBounded() {
    const std::string origin = "https://gateway.example";
    const std::string device_id = "device-test";
    const std::string token = "opaque-test-credential";
    const std::string user_id = "user-test";
    std::vector<uint8_t> frame{'V', 'L', 'I', '1'};
    AppendProvisionField(frame, origin);
    AppendProvisionField(frame, device_id);
    AppendProvisionField(frame, token);
    AppendProvisionField(frame, user_id);
    frame.insert(frame.end(), origin.begin(), origin.end());
    frame.insert(frame.end(), device_id.begin(), device_id.end());
    frame.insert(frame.end(), token.begin(), token.end());
    frame.insert(frame.end(), user_id.begin(), user_id.end());

    const auto header = voicelife::im::ParseImProvisioningHeader(frame);
    Check(header.ok() && header.value->payload_size == origin.size() + device_id.size() + token.size() + user_id.size(),
          "合法 provisioning header 必须给出有界 payload 大小");
    const auto parsed = voicelife::im::ParseImProvisioningRequest(frame);
    Check(parsed.ok() && parsed.value->gateway_origin == origin && parsed.value->device_id == device_id,
          "合法 provisioning frame 必须完整解析");
    Check(parsed.value->device_token == token && parsed.value->user_id == user_id,
          "解析结果必须保留凭据和可选用户引用");

    auto bad_magic = frame;
    bad_magic[0] = 'X';
    Check(voicelife::im::ParseImProvisioningHeader(bad_magic).status.code == ErrorCode::kInvalidArgument,
          "未知 provisioning magic 必须拒绝");
    auto truncated = frame;
    truncated.pop_back();
    Check(voicelife::im::ParseImProvisioningRequest(truncated).status.code == ErrorCode::kInvalidArgument,
          "截断 provisioning frame 必须拒绝");

    auto oversized = frame;
    oversized[8] = 0x02;
    oversized[9] = 0x01;
    Check(voicelife::im::ParseImProvisioningHeader(oversized).status.code == ErrorCode::kInvalidArgument,
          "超长 device token 必须在读取 payload 前拒绝");

    // 长度越界与非法内容分支。
    auto zero_origin = frame;
    zero_origin[4] = 0x00;
    zero_origin[5] = 0x00;
    Check(voicelife::im::ParseImProvisioningHeader(zero_origin).status.code == ErrorCode::kInvalidArgument,
          "零长度 gateway origin 必须拒绝");
    auto short_header = std::vector<uint8_t>(frame.begin(), frame.begin() + 8);
    Check(voicelife::im::ParseImProvisioningHeader(short_header).status.code == ErrorCode::kInvalidArgument,
          "短于固定头的输入必须拒绝");
    auto control_char = frame;
    control_char[12] = 0x01;
    Check(voicelife::im::ParseImProvisioningRequest(control_char).status.code == ErrorCode::kInvalidArgument,
          "含控制字符的字段必须拒绝");
    auto length_mismatch = frame;
    length_mismatch.push_back(0x00);
    Check(voicelife::im::ParseImProvisioningRequest(length_mismatch).status.code == ErrorCode::kInvalidArgument,
          "frame 长度与 header 不符必须拒绝");
}

void TestReadinessFailureDegradesWithoutTransport() {
    RuntimeFixture no_network;
    no_network.readiness.network_ready = false;
    Check(no_network.runtime.Start().code == ErrorCode::kUnavailable, "网络未就绪必须返回 unavailable");
    Check(no_network.runtime.state() == ImRuntimeState::kDegraded && no_network.factory_calls == 0,
          "网络未就绪不得构造 Transport");

    RuntimeFixture no_time;
    no_time.readiness.system_time_ready = false;
    Check(no_time.runtime.Start().code == ErrorCode::kUnavailable, "系统时间未就绪必须返回 unavailable");
    Check(no_time.runtime.state() == ImRuntimeState::kDegraded && no_time.factory_calls == 0,
          "时间未就绪不得构造 TLS Transport");
}

void TestReadyOwnsOneReportingChannel() {
    RuntimeFixture fixture;

    Check(fixture.runtime.Start().ok(), "有效配置应进入 ready");
    Check(fixture.runtime.state() == ImRuntimeState::kReady, "有效配置必须进入 ready");
    Check(fixture.factory_calls == 1 && fixture.last_origin == "https://gateway.example",
          "Runtime 必须用受控 origin 构造一次 Transport");
    Check(fixture.runtime.reporting_channel() != nullptr, "ready Runtime 必须持有上报通道");

    Check(fixture.runtime.Start().ok(), "重复启动应幂等成功");
    Check(fixture.factory_calls == 1, "重复启动不得创建重复 Transport 或任务");
}

void TestFactoryFailureIsDegraded() {
    FakeConfig config;
    FakeCredentials credentials;
    FakeReadiness readiness;
    ImRuntime runtime(config, credentials, readiness,
                      [](const std::string&) -> std::unique_ptr<ImTransport> { return nullptr; });

    Check(runtime.Start().code == ErrorCode::kUnavailable, "Transport factory 失败必须返回 unavailable");
    Check(runtime.state() == ImRuntimeState::kDegraded && runtime.reporting_channel() == nullptr,
          "Transport factory 失败不得留下半初始化上报通道");
}

void TestTrustedSystemTimeBoundary() {
    Check(!voicelife::im::IsTrustedSystemTime(0), "1970 默认 epoch 不得视为可信时间");
    Check(!voicelife::im::IsTrustedSystemTime(1704067199), "2024-01-01 前一秒不得视为可信时间");
    Check(voicelife::im::IsTrustedSystemTime(1704067200), "2024-01-01 参考时刻必须可信");
    Check(voicelife::im::IsTrustedSystemTime(4102444800), "未来时间必须可信");
}

void TestStoredConfigurationReadsSecretsOnlyWhenEnabled() {
    FakeSecretStore disabled_store;
    StoredImConfigProvider disabled(disabled_store, false);
    const auto disabled_config = disabled.Load();
    Check(disabled_config.ok() && !disabled_config.value->enabled, "禁用 Provider 必须返回 disabled 配置");
    Check(disabled_store.reads == 0 && disabled.DeviceId().empty() && disabled.DeviceToken().empty(),
          "禁用 Provider 不得读取任何 Secret");

    FakeSecretStore store;
    store.values = {{"gateway_origin", "https://gateway.example"},
                    {"device_id", "device-test"},
                    {"device_token", "token-test"},
                    {"user_id", "user-test"}};
    StoredImConfigProvider provider(store, true);
    const auto config = provider.Load();
    Check(config.ok() && config.value->enabled && config.value->gateway_origin == "https://gateway.example",
          "启用 Provider 必须加载 Gateway origin");
    Check(config.value->user_id == "user-test", "Provider 必须保留非 Secret 用户引用");
    Check(provider.DeviceId() == "device-test" && provider.DeviceToken() == "token-test",
          "Provider 必须向凭据端口提供设备身份");
}

void TestStoredConfigurationFailsClosedOnMissingFields() {
    FakeSecretStore store;
    store.values = {{"gateway_origin", "https://gateway.example"}, {"device_id", "device-test"}};
    StoredImConfigProvider provider(store, true);

    const auto config = provider.Load();
    Check(!config.ok() && config.status.code == ErrorCode::kNotFound, "缺 Token 的存储配置必须失败");
    Check(provider.DeviceId().empty() && provider.DeviceToken().empty(), "部分读取不得泄露半初始化凭据");

    store.values["device_token"] = "token-test";
    const auto without_user = provider.Load();
    Check(without_user.ok() && !without_user.value->user_id.has_value(), "userId 应保持可选");
    Check(provider.DeviceId() == "device-test" && provider.DeviceToken() == "token-test",
          "完整必填字段必须原子发布凭据");
}

void TestStoredConfigurationFailsClosedOnStoreReadFailure() {
    FakeSecretStore store;
    store.values = {
        {"gateway_origin", "https://gateway.example"}, {"device_id", "device-test"}, {"device_token", "token-test"}};
    store.fail_with = ErrorCode::kUnavailable;
    StoredImConfigProvider provider(store, true);

    const auto config = provider.Load();
    Check(!config.ok() && config.status.code == ErrorCode::kUnavailable, "存储读取失败必须返回 unavailable");
    Check(provider.DeviceId().empty() && provider.DeviceToken().empty(), "读取失败不得泄露凭据");
}

void TestStoredConfigurationFailsClosedOnUserIdReadFailure() {
    FakeSecretStore store;
    store.values = {{"gateway_origin", "https://gateway.example"},
                    {"device_id", "device-test"},
                    {"device_token", "token-test"},
                    {"user_id", "user-test"}};
    // 只让 user_id 读取失败，必填字段仍成功：验证 user_id 的失败分支 fail closed。
    store.fail_key = "user_id";
    StoredImConfigProvider provider(store, true);

    const auto config = provider.Load();
    Check(!config.ok() && config.status.code == ErrorCode::kUnavailable, "userId 读取失败必须 fail closed");
    Check(provider.DeviceId().empty() && provider.DeviceToken().empty(), "userId 失败不得发布凭据");
}

}  // namespace

int main() {
    TestDisabledDoesNotConstructTransport();
    TestMissingConfigurationFailsClosed();
    TestInvalidConfigurationAndCredentialsFailClosed();
    TestGatewayOriginValidationRejectsAmbiguousUrls();
    TestRetryPolicyIsBoundedAndStopsOnCredentials();
    TestRetryPolicyClampsAndHandles408();
    TestProvisioningFrameIsStrictAndBounded();
    TestReadinessFailureDegradesWithoutTransport();
    TestReadyOwnsOneReportingChannel();
    TestFactoryFailureIsDegraded();
    TestTrustedSystemTimeBoundary();
    TestStoredConfigurationReadsSecretsOnlyWhenEnabled();
    TestStoredConfigurationFailsClosedOnMissingFields();
    TestStoredConfigurationFailsClosedOnStoreReadFailure();
    TestStoredConfigurationFailsClosedOnUserIdReadFailure();
    std::cout << "PASS im_runtime_test\n";
    return EXIT_SUCCESS;
}
