// #235 平台无关微信公众号绑定用例：主机测试先于实现存在（TDD RED）。

#include <cstdint>
#include <string>
#include <utility>

#include "support/im_pairing_test_support.h"
#include "support/test_support.h"
#include "voicelife/im/im_binding_use_case.h"

using voicelife::im::BindingState;
using voicelife::im::BindingUseCase;
using voicelife::im::ImPairingClock;
using voicelife::im::PairingClientStatus;
using voicelife::test::Check;

namespace {

class FakeClock final : public ImPairingClock {
   public:
    uint64_t now_ms = 1000;
    uint64_t unix_ms = 1785715200000ULL;
    uint64_t MonotonicMillis() const override { return now_ms; }
    uint64_t UnixMillis() const override { return unix_ms; }
    void Advance(uint64_t milliseconds) {
        now_ms += milliseconds;
        unix_ms += milliseconds;
    }
};

voicelife::im::PairingQueryResult Query(std::string status) {
    auto session = CreatedSession("2026-08-03T00:00:00.000Z", "2026-08-03T00:05:00.000Z").session;
    session.status = std::move(status);
    if (session.status == "confirmed") session.confirmedAt = "2026-08-03T00:00:03.000Z";
    return {.status = PairingClientStatus::kSuccess, .value = std::move(session), .message = {}};
}

void Prepare(FakePairingPort& port) {
    port.created = {.status = PairingClientStatus::kSuccess,
                    .value = CreatedSession("2026-08-03T00:00:00.000Z", "2026-08-03T00:05:00.000Z"),
                    .message = {}};
}

void TestStartsAndRejectsDuplicateSession() {
    FakePairingPort port;
    FakeClock clock;
    Prepare(port);
    BindingUseCase use_case(port, clock);
    use_case.set_user_id("user-fixture");

    const auto started = use_case.Start(5);
    Check(started.state == BindingState::kPending && started.display_code == "123456" &&
              started.expires_at == "2026-08-03T00:05:00.000Z" && use_case.active(),
          "绑定用例应创建 pending 会话并返回六位码与到期时间");
    Check(use_case.Start().state == BindingState::kAlreadyActive && use_case.active(),
          "重复开始不得创建第二个 active 配对会话");
}

void TestObservesTerminalStates() {
    for (const auto& [wire_status, expected] : {
             std::pair{"confirmed", BindingState::kConfirmed},
             std::pair{"expired", BindingState::kExpired},
             std::pair{"cancelled", BindingState::kCancelled},
         }) {
        FakePairingPort port;
        FakeClock clock;
        Prepare(port);
        port.queried = {Query(wire_status)};
        BindingUseCase use_case(port, clock);
        use_case.set_user_id("user-fixture");
        Check(use_case.Start().state == BindingState::kPending, "终态测试必须先建立会话");
        clock.Advance(3000);
        Check(use_case.Poll().state == expected && !use_case.active(), "绑定用例必须观察终态并释放 active 会话");
    }
}

void TestMapsCreateAndQueryFailures() {
    {
        FakePairingPort port;
        FakeClock clock;
        port.created = {.status = PairingClientStatus::kCredentialRejected,
                        .value = std::nullopt,
                        .message = "credential rejected"};
        BindingUseCase use_case(port, clock);
        use_case.set_user_id("user-fixture");
        Check(use_case.Start().state == BindingState::kCredentialRejected, "创建期凭据拒绝应保持稳定业务分类");
    }
    {
        FakePairingPort port;
        FakeClock clock;
        port.created = {.status = PairingClientStatus::kRetryable, .value = std::nullopt, .message = "network"};
        BindingUseCase use_case(port, clock);
        use_case.set_user_id("user-fixture");
        Check(use_case.Start().state == BindingState::kFailed && !use_case.active(),
              "创建期网络失败应立即收敛为创建失败");
    }
    {
        FakePairingPort port;
        FakeClock clock;
        Prepare(port);
        port.queried = {{.status = PairingClientStatus::kRetryable, .value = std::nullopt, .message = "network"}};
        BindingUseCase use_case(port, clock);
        use_case.set_user_id("user-fixture");
        Check(use_case.Start().state == BindingState::kPending, "查询失败测试必须先建立会话");
        clock.Advance(3000);
        Check(use_case.Poll().state == BindingState::kRetrying && use_case.active(),
              "查询期网络失败应进入有限退避并保留 active 会话");
    }
}

void TestRequiresReadyRuntimeAndUser() {
    BindingUseCase unavailable;
    Check(unavailable.Start().state == BindingState::kUnavailable, "未注入配对端口时绑定功能应明确不可用");
    Check(unavailable.Poll().state == BindingState::kUnavailable, "未就绪时 Poll 必须保持 unavailable 且不重查");

    FakePairingPort port;
    FakeClock clock;
    Prepare(port);
    BindingUseCase missing_user(port, clock);
    Check(missing_user.Start().state == BindingState::kUnavailable, "缺少 user_id 时不得创建配对会话");
}

void TestObservesWaitingNotFoundAndTimedOut() {
    {
        FakePairingPort port;
        FakeClock clock;
        Prepare(port);
        BindingUseCase use_case(port, clock);
        use_case.set_user_id("user-fixture");
        Check(use_case.Start().state == BindingState::kPending, "waiting 测试必须先建立会话");
        Check(use_case.Poll().state == BindingState::kWaiting && use_case.active(),
              "轮询间隔未到必须返回 waiting 且保持 active");
    }
    {
        FakePairingPort port;
        FakeClock clock;
        Prepare(port);
        port.queried = {{.status = PairingClientStatus::kNotFound, .value = std::nullopt, .message = "not found"}};
        BindingUseCase use_case(port, clock);
        use_case.set_user_id("user-fixture");
        Check(use_case.Start().state == BindingState::kPending, "not_found 测试必须先建立会话");
        clock.Advance(3000);
        Check(use_case.Poll().state == BindingState::kNotFound && !use_case.active(),
              "查询 404 必须收敛为 not_found 终态");
    }
    {
        FakePairingPort port;
        FakeClock clock;
        Prepare(port);
        port.queried = {Query("pending")};
        BindingUseCase use_case(port, clock);
        use_case.set_user_id("user-fixture");
        Check(use_case.Start().state == BindingState::kPending, "timed_out 测试必须先建立会话");
        clock.Advance(300000);
        Check(use_case.Poll().state == BindingState::kTimedOut && !use_case.active(),
              "到达本地截止时间必须收敛为 timed_out 终态");
    }
}

void TestPollAfterTerminalStaysIdle() {
    FakePairingPort port;
    FakeClock clock;
    Prepare(port);
    port.queried = {Query("confirmed")};
    BindingUseCase use_case(port, clock);
    use_case.set_user_id("user-fixture");
    Check(use_case.Start().state == BindingState::kPending, "终态后 Poll 测试必须先建立会话");
    clock.Advance(3000);
    Check(use_case.Poll().state == BindingState::kConfirmed && !use_case.active(), "必须先进入 confirmed 终态");
    Check(use_case.Poll().state == BindingState::kConfirmed, "终态后再 Poll 必须保持终态而不重查");
}

void TestRebindClearsSessionAndTerminalAllowsRestart() {
    FakePairingPort first;
    FakePairingPort second;
    FakeClock clock;
    Prepare(first);
    Prepare(second);
    first.queried = {Query("confirmed")};
    BindingUseCase use_case(first, clock);
    use_case.set_user_id("user-fixture");
    Check(use_case.Start().state == BindingState::kPending, "重绑定测试必须先建立会话");
    use_case.Bind(second, clock, "user-fixture");
    Check(!use_case.active() && use_case.state() == BindingState::kIdle, "重新注入 Runtime 必须清理旧 active 会话");

    Check(use_case.Start().state == BindingState::kPending, "重新注入后应允许创建新会话");
    second.queried = {Query("confirmed")};
    clock.Advance(3000);
    Check(use_case.Poll().state == BindingState::kConfirmed, "新会话应能正常进入终态");
    Prepare(second);
    Check(use_case.Start().state == BindingState::kPending, "终态后应允许显式开始下一次绑定");
}

}  // namespace

int main() {
    TestStartsAndRejectsDuplicateSession();
    TestObservesTerminalStates();
    TestMapsCreateAndQueryFailures();
    TestRequiresReadyRuntimeAndUser();
    TestObservesWaitingNotFoundAndTimedOut();
    TestPollAfterTerminalStaysIdle();
    TestRebindClearsSessionAndTerminalAllowsRestart();
    return 0;
}
