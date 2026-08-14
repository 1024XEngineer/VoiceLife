// #235 IM 平台绑定用例：并发安全、already_active 携带当前码、有效期与绑定码校验。

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
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

void TestAlreadyActiveReturnsCurrentCode() {
    FakePairingPort port;
    FakeClock clock;
    Prepare(port);
    BindingUseCase use_case(port, clock);
    use_case.set_user_id("user-fixture");
    Check(use_case.Start().state == BindingState::kPending, "重复开始测试必须先建立会话");

    const auto again = use_case.Start();
    Check(again.state == BindingState::kAlreadyActive && again.display_code == "123456" &&
              again.expires_at == "2026-08-03T00:05:00.000Z",
          "already_active 必须携带当前六位码与到期时间，保证「使用当前绑定码」可执行");
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

void TestRebindInvalidatesResultsFromThePreviousRuntime() {
    FakePairingPort first;
    FakePairingPort second;
    FakeClock clock;
    Prepare(first);
    Prepare(second);
    first.queried = {Query("confirmed")};
    BindingUseCase use_case(first, clock);
    use_case.set_user_id("user-fixture");

    const auto started = use_case.Start();
    Check(started.generation != 0 && started.expires_in_minutes == 10,
          "创建会话必须生成可用于丢弃旧结果的代次并保留有效期");
    clock.Advance(3000);
    const auto terminal = use_case.Poll();
    use_case.Bind(second, clock, "user-fixture");

    Check(terminal.state == BindingState::kConfirmed && terminal.generation != use_case.generation(),
          "Runtime 重绑后，旧会话的终态结果必须能由其旧代次识别并丢弃");
    const auto restarted = use_case.Start(5);
    Check(restarted.state == BindingState::kPending && restarted.generation == use_case.generation() &&
              restarted.expires_in_minutes == 5,
          "重启策略必须清理本地会话并要求下一次显式开始，新的会话使用新代次");
}

void TestRejectsOutOfRangeExpiry() {
    {
        FakePairingPort port;
        FakeClock clock;
        // 服务端会话窗口必须与请求时长匹配（控制器拒绝“服务端窗口超过请求上限”）。
        port.created = {.status = PairingClientStatus::kSuccess,
                        .value = CreatedSession("2026-08-03T00:00:00.000Z", "2026-08-03T00:01:00.000Z"),
                        .message = {}};
        BindingUseCase use_case(port, clock);
        use_case.set_user_id("user-fixture");
        for (const int invalid : {0, -1, 11, 100}) {
            const auto result = use_case.Start(invalid);
            Check(result.state == BindingState::kFailed && !use_case.active() && result.display_code.empty(),
                  "越界有效期必须直接失败，不得静默截断或创建会话");
        }
        Check(use_case.Start(1).state == BindingState::kPending, "下边界 1 分钟应可用");
    }
    {
        FakePairingPort port;
        FakeClock clock;
        port.created = {.status = PairingClientStatus::kSuccess,
                        .value = CreatedSession("2026-08-03T00:00:00.000Z", "2026-08-03T00:10:00.000Z"),
                        .message = {}};
        BindingUseCase use_case(port, clock);
        use_case.set_user_id("user-fixture");
        Check(use_case.Start(10).state == BindingState::kPending, "上边界 10 分钟应可用");
    }
}

void TestRejectsMalformedDisplayCode() {
    for (const char* bad : {"", "12345", "1234567", "12345a", "abcdef"}) {
        FakePairingPort port;
        FakeClock clock;
        auto session = CreatedSession("2026-08-03T00:00:00.000Z", "2026-08-03T00:05:00.000Z");
        session.displayCode = bad;
        port.created = {.status = PairingClientStatus::kSuccess, .value = std::move(session), .message = {}};
        BindingUseCase use_case(port, clock);
        use_case.set_user_id("user-fixture");
        const auto result = use_case.Start();
        Check(result.state == BindingState::kFailed && !use_case.active(),
              "服务端返回非法绑定码必须立即失败且不得进入 active");
    }
}

void TestConcurrentBindAndStart() {
    FakePairingPort port;
    FakeClock clock;
    Prepare(port);
    // 预置足够多的 pending 查询响应，供并发轮询消费，避免假端口查询枯竭。
    port.queried.assign(20000, Query("pending"));
    BindingUseCase use_case(port, clock);

    std::atomic<unsigned> starter_iterations{0};
    std::atomic<unsigned> poller_iterations{0};
    std::thread starter([&] {
        while (starter_iterations.fetch_add(1) < 3000) (void)use_case.Start(5);
    });
    std::thread poller([&] {
        while (poller_iterations.fetch_add(1) < 3000) {
            (void)use_case.Poll();
            (void)use_case.active();
            (void)use_case.state();
        }
    });
    for (int index = 0; index < 300; ++index) {
        use_case.Bind(port, clock, "user-fixture");
    }
    starter.join();
    poller.join();
    Check(true, "并发 Bind/Start/Poll/active/state 必须无崩溃、无死锁");
}

}  // namespace

int main() {
    TestStartsAndRejectsDuplicateSession();
    TestAlreadyActiveReturnsCurrentCode();
    TestObservesTerminalStates();
    TestMapsCreateAndQueryFailures();
    TestRequiresReadyRuntimeAndUser();
    TestObservesWaitingNotFoundAndTimedOut();
    TestPollAfterTerminalStaysIdle();
    TestRebindClearsSessionAndTerminalAllowsRestart();
    TestRebindInvalidatesResultsFromThePreviousRuntime();
    TestRejectsOutOfRangeExpiry();
    TestRejectsMalformedDisplayCode();
    TestConcurrentBindAndStart();
    return 0;
}
