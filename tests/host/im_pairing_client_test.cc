// #234 设备配对客户端与有限轮询：主机测试先于实现存在（TDD RED）。

#include "voicelife/im/im_pairing_client.h"

#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "support/test_support.h"
#include "voicelife/contracts/im/pairing_session.h"
#include "voicelife/contracts/json.h"
#include "voicelife/im/im_credentials.h"
#include "voicelife/im/im_pairing_controller.h"
#include "voicelife/im/im_transport.h"

using voicelife::im::ImCredentialProvider;
using voicelife::im::ImHttpHeader;
using voicelife::im::ImHttpRequest;
using voicelife::im::ImHttpResponse;
using voicelife::im::ImPairingClient;
using voicelife::im::ImPairingClock;
using voicelife::im::ImTransport;
using voicelife::im::ImTransportStatus;
using voicelife::im::PairingClientStatus;
using voicelife::im::PairingFlowStatus;
using voicelife::im::PairingSessionController;
using voicelife::test::Check;

namespace {

std::string ReadFixture(const char* name) {
    std::ifstream input(std::string(VOICELIFE_SOURCE_DIR) + "/contracts/im-gateway/v1/fixtures/" + name);
    Check(input.good(), "共享配对 fixture 必须存在");
    std::ostringstream content;
    content << input.rdbuf();
    return content.str();
}

std::string HeaderValue(const ImHttpRequest& request, const std::string& name) {
    for (const ImHttpHeader& header : request.headers) {
        if (header.name == name) return header.value;
    }
    return {};
}

class FakeTransport final : public ImTransport {
   public:
    std::vector<ImHttpRequest> requests;
    std::vector<ImHttpResponse> responses;

    ImHttpResponse Post(const ImHttpRequest& request) override { return Respond(request); }
    ImHttpResponse Get(const ImHttpRequest& request) override { return Respond(request); }

   private:
    ImHttpResponse Respond(const ImHttpRequest& request) {
        requests.push_back(request);
        Check(!responses.empty(), "假传输必须预置响应");
        ImHttpResponse response = responses.front();
        responses.erase(responses.begin());
        return response;
    }
};

class FakeCredentials final : public ImCredentialProvider {
   public:
    std::string token = "fixture-device-token";
    std::string device_id = "device-fixture";
    std::string DeviceToken() const override { return token; }
    std::string DeviceId() const override { return device_id; }
};

class FakeClock final : public ImPairingClock {
   public:
    uint64_t now_ms = 1000;
    uint64_t MonotonicMillis() const override { return now_ms; }
};

ImHttpResponse Response(ImTransportStatus status, int code, std::string body = {}) {
    return {.status = status, .status_code = code, .body = std::move(body), .message = "fake"};
}

void TestClientCreatesAndQueriesWithAuthenticatedContract() {
    FakeTransport transport;
    FakeCredentials credentials;
    transport.responses = {
        Response(ImTransportStatus::kSuccess, 201, ReadFixture("pairing-created.json")),
        Response(
            ImTransportStatus::kSuccess, 200,
            R"({"id":"pairing/../?secret#fragment","userId":"user-fixture","deviceId":"device-fixture","allowedPlatforms":["wechat_official"],"status":"pending","expiresAt":"2026-08-03T00:05:00.000Z","createdAt":"2026-08-03T00:00:00.000Z"})"),
    };
    ImPairingClient client(transport, credentials);

    const auto created = client.Create({.user_id = "user-fixture", .expires_in_minutes = 5});
    Check(created.status == PairingClientStatus::kSuccess && created.value.has_value(), "合法创建响应必须返回配对码");
    Check(created.value->displayCode == "123456", "必须保留六位展示码");
    Check(transport.requests.size() == 1 && transport.requests[0].method == "POST" &&
              transport.requests[0].path == "/v1/im/pairing-sessions",
          "创建必须 POST 固定配对路径");
    Check(HeaderValue(transport.requests[0], "Authorization") == "Bearer fixture-device-token" &&
              HeaderValue(transport.requests[0], "Content-Type") == "application/json",
          "创建必须使用 Bearer 和 JSON 头");
    voicelife::JsonValue request_json;
    voicelife::contracts::im::CreatePairingSessionRequest request_contract;
    Check(voicelife::ParseJson(transport.requests[0].body, request_json).ok() &&
              voicelife::contracts::im::ParseCreatePairingSessionRequest(request_json, request_contract).ok() &&
              request_contract.userId == "user-fixture" && request_contract.deviceId == "device-fixture" &&
              request_contract.allowedPlatforms.has_value() &&
              *request_contract.allowedPlatforms == std::vector<std::string>{"wechat_official"} &&
              request_contract.expiresInMinutes == 5,
          "创建载荷必须固定 wechat_official 并符合共享契约");

    const auto queried = client.Query("pairing/../?secret#fragment");
    Check(queried.status == PairingClientStatus::kSuccess && queried.value.has_value(),
          "合法查询响应必须返回 pending 状态");
    Check(transport.requests.size() == 2 && transport.requests[1].method == "GET", "状态查询必须使用 GET");
    Check(transport.requests[1].path == "/v1/im/pairing-sessions/pairing%2F..%2F%3Fsecret%23fragment",
          "session id 必须作为单一 path segment 百分号编码");
    Check(HeaderValue(transport.requests[1], "Authorization") == "Bearer fixture-device-token",
          "查询必须沿用相同 Bearer 设备身份");
}

void TestClientFailsClosedAndClassifiesErrors() {
    FakeTransport transport;
    FakeCredentials credentials;
    ImPairingClient client(transport, credentials);

    transport.responses.push_back(Response(ImTransportStatus::kCredentialRejected, 401));
    Check(client.Query("pairing-1").status == PairingClientStatus::kCredentialRejected,
          "401/403 必须稳定映射为凭据拒绝");
    transport.responses.push_back(Response(ImTransportStatus::kHttpError, 404));
    Check(client.Query("pairing-1").status == PairingClientStatus::kNotFound, "认证后的 404 必须稳定映射为 not found");
    transport.responses.push_back(Response(ImTransportStatus::kHttpError, 503));
    Check(client.Query("pairing-1").status == PairingClientStatus::kRetryable, "5xx 必须稳定映射为可重试");
    transport.responses.push_back(Response(ImTransportStatus::kHttpError, 400));
    Check(client.Query("pairing-1").status == PairingClientStatus::kRejected, "非限流 4xx 必须稳定映射为不可重试拒绝");
    transport.responses.push_back(Response(ImTransportStatus::kInvalidConfig, 0));
    Check(client.Query("pairing-1").status == PairingClientStatus::kRejected,
          "本地 Transport 配置错误必须稳定映射为不可重试拒绝");
    transport.responses.push_back(Response(ImTransportStatus::kSuccess, 200, R"({"id":"pairing-1")"));
    Check(client.Query("pairing-1").status == PairingClientStatus::kInvalidResponse,
          "截断响应必须 fail closed，不得误判成功");
    transport.responses.push_back(Response(
        ImTransportStatus::kSuccess, 200,
        R"({"id":"pairing-1","deviceId":"another-device","status":"confirmed","expiresAt":"2026-08-03T00:10:00.000Z","createdAt":"2026-08-03T00:00:00.000Z","confirmedAt":"2026-08-03T00:01:00.000Z"})"));
    Check(client.Query("pairing-1").status == PairingClientStatus::kInvalidResponse,
          "跨设备响应必须 fail closed，不能误判 confirmed");
    transport.responses.push_back(
        Response(ImTransportStatus::kSuccess, 200, ReadFixture("pairing-status-invalid-secret.json")));
    Check(client.Query("pairing-1").status == PairingClientStatus::kInvalidResponse,
          "含内部 hash 的响应必须 fail closed");

    credentials.token.clear();
    const std::size_t before = transport.requests.size();
    Check(client.Create({}).status == PairingClientStatus::kCredentialRejected, "未配置 Token 时必须本地拒绝");
    Check(transport.requests.size() == before, "本地凭据错误不得发出网络请求");
}

void TestClientEscapesJsonAndRejectsInvalidInputsLocally() {
    FakeTransport transport;
    FakeCredentials credentials;
    transport.responses = {Response(ImTransportStatus::kSuccess, 201, ReadFixture("pairing-created.json"))};
    ImPairingClient client(transport, credentials);

    std::string special_user_id = "quote\" slash\\ backspace\b formfeed\f newline\n return\r tab\t control";
    special_user_id.push_back('\x01');
    const auto created = client.Create({.user_id = special_user_id, .expires_in_minutes = 5});
    Check(created.status == PairingClientStatus::kSuccess, "含 JSON 特殊字符的 user id 必须可安全序列化");
    voicelife::JsonValue request_json;
    voicelife::contracts::im::CreatePairingSessionRequest request_contract;
    Check(voicelife::ParseJson(transport.requests[0].body, request_json).ok() &&
              voicelife::contracts::im::ParseCreatePairingSessionRequest(request_json, request_contract).ok() &&
              request_contract.userId == special_user_id,
          "JSON 转义后必须无损还原全部特殊字符");

    const std::size_t before = transport.requests.size();
    Check(client.Create({.user_id = std::nullopt, .expires_in_minutes = 0}).status == PairingClientStatus::kRejected,
          "零分钟配对窗口必须本地拒绝");
    Check(client.Create({.user_id = std::nullopt, .expires_in_minutes = 11}).status == PairingClientStatus::kRejected,
          "超过上限的配对窗口必须本地拒绝");
    Check(client.Create({.user_id = "", .expires_in_minutes = 5}).status == PairingClientStatus::kRejected,
          "显式空 user id 必须本地拒绝");
    Check(client.Query("").status == PairingClientStatus::kRejected, "空 session id 必须本地拒绝");
    Check(transport.requests.size() == before, "非法输入不得发出网络请求");

    credentials.device_id.clear();
    Check(client.Query("pairing-1").status == PairingClientStatus::kCredentialRejected,
          "缺少 device id 时查询必须本地拒绝");
    Check(transport.requests.size() == before, "缺少设备身份不得发出查询请求");
}

void TestClientRejectsCreateAndQueryFailures() {
    FakeTransport transport;
    FakeCredentials credentials;
    transport.responses = {
        Response(ImTransportStatus::kHttpError, 503),
        Response(ImTransportStatus::kSuccess, 201, R"({"session":{}})"),
        Response(
            ImTransportStatus::kSuccess, 200,
            R"({"id":"pairing-1","deviceId":"device-fixture","status":"confirmed","expiresAt":"2026-08-03T00:10:00.000Z","createdAt":"2026-08-03T00:00:00.000Z"})"),
    };
    ImPairingClient client(transport, credentials);

    Check(client.Create({}).status == PairingClientStatus::kRetryable, "创建时服务端 5xx 必须映射为可重试");
    Check(client.Create({}).status == PairingClientStatus::kInvalidResponse, "非法创建响应必须 fail closed");
    Check(client.Query("pairing-1").status == PairingClientStatus::kInvalidResponse,
          "confirmed 与 confirmedAt 不一致的查询响应必须 fail closed");
}

void TestControllerPollsFinitelyAndRejectsDuplicateStart() {
    FakeTransport transport;
    FakeCredentials credentials;
    FakeClock clock;
    transport.responses = {
        Response(ImTransportStatus::kSuccess, 201, ReadFixture("pairing-created.json")),
        Response(ImTransportStatus::kSuccess, 200, ReadFixture("pairing-status.json")),
        Response(ImTransportStatus::kNetworkFailure, 0),
        Response(ImTransportStatus::kSuccess, 200, ReadFixture("pairing-status-confirmed.json")),
    };
    ImPairingClient client(transport, credentials);
    PairingSessionController controller(client, clock);

    auto result = controller.Begin({.user_id = "user-fixture", .expires_in_minutes = 10});
    Check(result.status == PairingFlowStatus::kPending && result.display_code == "123456" && controller.active(),
          "开始后必须暴露展示码并保持唯一 active session");
    Check(controller.Begin({}).status == PairingFlowStatus::kAlreadyActive && transport.requests.size() == 1,
          "重复开始必须拒绝且不得创建第二个会话");

    clock.now_ms += 2999;
    Check(controller.Poll().status == PairingFlowStatus::kWaiting && transport.requests.size() == 1,
          "轮询间隔未到不得请求");
    clock.now_ms += 1;
    Check(controller.Poll().status == PairingFlowStatus::kPending && transport.requests.size() == 2,
          "pending 必须按三秒间隔查询");
    clock.now_ms += 3000;
    Check(controller.Poll().status == PairingFlowStatus::kRetrying && controller.active(),
          "临时网络失败必须有限退避且保留 active session");
    clock.now_ms += 1999;
    Check(controller.Poll().status == PairingFlowStatus::kWaiting && transport.requests.size() == 3,
          "退避窗口内不得紧密重试");
    clock.now_ms += 1;
    Check(controller.Poll().status == PairingFlowStatus::kConfirmed && !controller.active(),
          "confirmed 必须立即停止并释放 active session");
}

void TestControllerStopsOnTerminalAndDeadline() {
    for (const auto& [fixture, expected] : std::vector<std::pair<const char*, PairingFlowStatus>>{
             {"pairing-status-confirmed.json", PairingFlowStatus::kConfirmed},
             {"pairing-status-expired.json", PairingFlowStatus::kExpired},
             {"pairing-status-cancelled.json", PairingFlowStatus::kCancelled}}) {
        FakeTransport transport;
        FakeCredentials credentials;
        FakeClock clock;
        transport.responses = {Response(ImTransportStatus::kSuccess, 201, ReadFixture("pairing-created.json")),
                               Response(ImTransportStatus::kSuccess, 200, ReadFixture(fixture))};
        ImPairingClient client(transport, credentials);
        PairingSessionController controller(client, clock);
        Check(controller.Begin({}).status == PairingFlowStatus::kPending, "测试会话必须创建成功");
        clock.now_ms += 3000;
        Check(controller.Poll().status == expected && !controller.active(), "终态必须立即释放 active session");
    }

    FakeTransport transport;
    FakeCredentials credentials;
    FakeClock clock;
    transport.responses = {Response(ImTransportStatus::kSuccess, 201, ReadFixture("pairing-created.json"))};
    ImPairingClient client(transport, credentials);
    PairingSessionController controller(client, clock);
    Check(controller.Begin({.user_id = std::nullopt, .expires_in_minutes = 1}).status == PairingFlowStatus::kPending,
          "一分钟会话必须创建成功");
    clock.now_ms += 60000;
    Check(controller.Poll().status == PairingFlowStatus::kTimedOut && !controller.active() &&
              transport.requests.size() == 1,
          "本地截止时间到达必须无请求停止");
}

void TestControllerStopsOnNotFoundCredentialsAndRetryExhaustion() {
    for (const auto& [response, expected] : std::vector<std::pair<ImHttpResponse, PairingFlowStatus>>{
             {Response(ImTransportStatus::kHttpError, 404), PairingFlowStatus::kNotFound},
             {Response(ImTransportStatus::kCredentialRejected, 401), PairingFlowStatus::kCredentialRejected},
         }) {
        FakeTransport transport;
        FakeCredentials credentials;
        FakeClock clock;
        transport.responses = {Response(ImTransportStatus::kSuccess, 201, ReadFixture("pairing-created.json")),
                               response};
        ImPairingClient client(transport, credentials);
        PairingSessionController controller(client, clock);
        Check(controller.Begin({}).status == PairingFlowStatus::kPending, "测试会话必须创建成功");
        clock.now_ms += 3000;
        Check(controller.Poll().status == expected && !controller.active(),
              "404 或凭据拒绝必须立即停止并释放 active session");
    }

    FakeTransport transport;
    FakeCredentials credentials;
    FakeClock clock;
    transport.responses = {Response(ImTransportStatus::kSuccess, 201, ReadFixture("pairing-created.json"))};
    for (int attempt = 0; attempt < 5; ++attempt) {
        transport.responses.push_back(Response(ImTransportStatus::kNetworkFailure, 0));
    }
    ImPairingClient client(transport, credentials);
    PairingSessionController controller(client, clock);
    Check(controller.Begin({}).status == PairingFlowStatus::kPending, "测试会话必须创建成功");
    for (const uint64_t delay : std::vector<uint64_t>{3000, 2000, 4000, 5000}) {
        clock.now_ms += delay;
        Check(controller.Poll().status == PairingFlowStatus::kRetrying && controller.active(),
              "上限内网络失败必须使用有限退避");
    }
    clock.now_ms += 5000;
    Check(controller.Poll().status == PairingFlowStatus::kFailed && !controller.active(),
          "重试预算耗尽后必须停止并释放 active session");
}

void TestControllerStopsWhenCreationOrQueryIsRejected() {
    {
        FakeTransport transport;
        FakeCredentials credentials;
        FakeClock clock;
        credentials.token.clear();
        ImPairingClient client(transport, credentials);
        PairingSessionController controller(client, clock);
        Check(controller.Begin({}).status == PairingFlowStatus::kCredentialRejected && !controller.active(),
              "创建凭据被拒绝时控制器必须返回专用终态");
    }
    {
        FakeTransport transport;
        FakeCredentials credentials;
        FakeClock clock;
        transport.responses = {Response(ImTransportStatus::kHttpError, 400)};
        ImPairingClient client(transport, credentials);
        PairingSessionController controller(client, clock);
        Check(controller.Begin({}).status == PairingFlowStatus::kFailed && !controller.active(),
              "创建请求被拒绝时控制器必须停止");
    }
    {
        FakeTransport transport;
        FakeCredentials credentials;
        FakeClock clock;
        transport.responses = {
            Response(ImTransportStatus::kSuccess, 201, ReadFixture("pairing-created.json")),
            Response(ImTransportStatus::kSuccess, 200, R"({"id":"pairing-1"})"),
        };
        ImPairingClient client(transport, credentials);
        PairingSessionController controller(client, clock);
        Check(controller.Begin({}).status == PairingFlowStatus::kPending, "异常查询测试必须先创建会话");
        clock.now_ms += 3000;
        Check(controller.Poll().status == PairingFlowStatus::kFailed && !controller.active(),
              "非法查询响应必须停止控制器并释放 active session");
    }
}

}  // namespace

int main() {
    TestClientCreatesAndQueriesWithAuthenticatedContract();
    TestClientFailsClosedAndClassifiesErrors();
    TestClientEscapesJsonAndRejectsInvalidInputsLocally();
    TestClientRejectsCreateAndQueryFailures();
    TestControllerPollsFinitelyAndRejectsDuplicateStart();
    TestControllerStopsOnTerminalAndDeadline();
    TestControllerStopsOnNotFoundCredentialsAndRetryExhaustion();
    TestControllerStopsWhenCreationOrQueryIsRejected();
    return 0;
}
