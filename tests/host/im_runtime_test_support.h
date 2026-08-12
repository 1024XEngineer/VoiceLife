#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "voicelife/im/im_config_store.h"
#include "voicelife/im/im_runtime.h"

namespace voicelife::test::im_runtime_support {

class FakeConfig final : public im::ImConfigProvider {
   public:
    Result<im::ImRuntimeConfig> result = Result<im::ImRuntimeConfig>::Success(
        {.enabled = true, .gateway_origin = "https://gateway.example", .user_id = "user-test"});

    Result<im::ImRuntimeConfig> Load() override { return result; }
};

class FakeCredentials final : public im::ImCredentialProvider {
   public:
    std::string token = "device-token";
    std::string device_id = "device-test";

    std::string DeviceToken() const override { return token; }
    std::string DeviceId() const override { return device_id; }
};

class FakeReadiness final : public im::ImRuntimeReadinessPort {
   public:
    bool network_ready = true;
    bool system_time_ready = true;

    bool NetworkReady() const override { return network_ready; }
    bool SystemTimeReady() const override { return system_time_ready; }
};

class FakeTransport final : public im::ImTransport {
   public:
    im::ImHttpResponse Post(const im::ImHttpRequest&) override { return {}; }
    im::ImHttpResponse Get(const im::ImHttpRequest& request) override {
        ++get_calls;
        last_request = request;
        return next_get_response;
    }

    int get_calls = 0;
    im::ImHttpRequest last_request;
    im::ImHttpResponse next_get_response{
        .status = im::ImTransportStatus::kHttpError,
        .status_code = 404,
        .body = "Not Found",
        .message = "404",
    };
};

class FakeSecretStore final : public im::ImSecretStorePort {
   public:
    Result<std::string> Read(std::string_view key) override {
        ++reads;
        if (fail_key.has_value() && key == *fail_key) {
            return Result<std::string>::Failure(ErrorCode::kUnavailable, "read failure");
        }
        if (fail_with == ErrorCode::kNotFound) {
            return Result<std::string>::Failure(ErrorCode::kNotFound, "missing");
        }
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
    FakeTransport* transport = nullptr;
    im::ImRuntime runtime{config, credentials, readiness, [this](const std::string& origin) {
                              ++factory_calls;
                              last_origin = origin;
                              auto created = std::make_unique<FakeTransport>();
                              transport = created.get();
                              return created;
                          }};
    std::string last_origin;
};

}  // namespace voicelife::test::im_runtime_support
