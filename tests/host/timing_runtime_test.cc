#include "voicelife/timing/timing_runtime.h"

#include <chrono>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "support/test_support.h"

using voicelife::ErrorCode;
using voicelife::Status;
using voicelife::test::Check;
using namespace std::chrono_literals;
using namespace voicelife::timing;

namespace {

TaskId RequireTaskId(std::string value) {
    auto task_id = TaskId::Create(std::move(value));
    Check(task_id.has_value(), "test task id should be valid");
    return std::move(*task_id);
}

class FixedClock final : public ClockPort {
   public:
    explicit FixedClock(TriggerAt now) : now_(now) {}

    TriggerAt Now() override { return now_; }
    void SetNow(TriggerAt now) { now_ = now; }

   private:
    TriggerAt now_;
};

class SequenceClock final : public ClockPort {
   public:
    explicit SequenceClock(std::vector<TriggerAt> readings) : readings_(std::move(readings)) {}

    TriggerAt Now() override {
        Check(next_reading_ < readings_.size(), "runtime should not read the clock more often than expected");
        return readings_[next_reading_++];
    }

   private:
    std::vector<TriggerAt> readings_;
    size_t next_reading_ = 0;
};

class RecordingOneShotTimer final : public OneShotTimerPort {
   public:
    void SetExpiryCallback(ExpiryCallback callback) override { callback_ = std::move(callback); }
    void ClearExpiryCallbackAndWait() override {
        callback_ = {};
        ++clear_count;
    }
    Status ArmAfter(std::chrono::microseconds delay) override {
        armed_delays.push_back(delay);
        return arm_result;
    }
    Status Disarm() override {
        ++disarm_count;
        return disarm_result;
    }
    void Fire() {
        Check(static_cast<bool>(callback_), "timer expiry callback should be configured");
        callback_();
    }
    void FireIfBound() {
        if (callback_) {
            callback_();
        }
    }

    std::vector<std::chrono::microseconds> armed_delays;
    size_t disarm_count = 0;
    size_t clear_count = 0;
    Status arm_result = Status::Ok();
    Status disarm_result = Status::Ok();

   private:
    ExpiryCallback callback_;
};

class RecordingRunnerWake final : public RunnerWakePort {
   public:
    void Notify() override { ++notification_count; }

    size_t notification_count = 0;
};

RegisterTaskCommand Registration(std::string task_id, TriggerAt trigger_at) {
    return {
        .task_id = RequireTaskId(std::move(task_id)),
        .trigger_at = trigger_at,
        .callback = [](const TaskId&, TriggerAt) {},
        .on_result = [](RegisterTaskResult) {},
    };
}

void ArmsOnlyTheEarliestPendingTaskAfterRegistrationWake() {
    InMemoryTimingTaskRunner runner;
    FixedClock clock(TriggerAt{2h});
    RecordingOneShotTimer timer;
    RecordingRunnerWake wake;
    TimingTaskRuntime runtime(runner, clock, timer, wake);

    runtime.RegisterTask(Registration("later", TriggerAt{5h}));
    runtime.RegisterTask(Registration("earlier", TriggerAt{3h}));

    Check(wake.notification_count == 2, "each external registration should wake the Runner");
    runtime.ProcessWake();

    Check(timer.armed_delays == std::vector<std::chrono::microseconds>{1h},
          "Runner should arm one timer for the earliest pending task");
    Check(timer.disarm_count == 0, "pending tasks should keep the one-shot timer armed");
}

void DisarmsTimerWhenNoTaskRemainsPending() {
    InMemoryTimingTaskRunner runner;
    FixedClock clock(TriggerAt{2h});
    RecordingOneShotTimer timer;
    RecordingRunnerWake wake;
    TimingTaskRuntime runtime(runner, clock, timer, wake);
    Check(timer.ArmAfter(1h).ok(), "test setup should arm the timer");

    runtime.ProcessWake();

    Check(timer.disarm_count == 1, "empty pending registry should disarm the one-shot timer");
    Check(timer.armed_delays == std::vector<std::chrono::microseconds>{1h},
          "empty pending registry should not arm another wake");
}

void ClampsOverdueDeferredTaskToZeroDelay() {
    InMemoryTimingTaskRunner runner;
    FixedClock clock(TriggerAt{2h});
    RecordingOneShotTimer timer;
    RecordingRunnerWake wake;
    TimingTaskRuntime runtime(runner, clock, timer, wake);
    runtime.RegisterTask({
        .task_id = RequireTaskId("task-1"),
        .trigger_at = TriggerAt{2h},
        .callback = [&runtime](const TaskId&,
                               TriggerAt) { runtime.RegisterTask(Registration("task-2", TriggerAt{1h})); },
        .on_result = [](RegisterTaskResult) {},
    });

    runtime.ProcessWake();

    Check(timer.armed_delays == std::vector<std::chrono::microseconds>{0us},
          "overdue task deferred by the active batch should arm a zero delay");
}

void TimerExpiryOnlyWakesRunnerUntilNormalContextProcessesIt() {
    InMemoryTimingTaskRunner runner;
    FixedClock clock(TriggerAt{1h});
    RecordingOneShotTimer timer;
    RecordingRunnerWake wake;
    TimingTaskRuntime runtime(runner, clock, timer, wake);
    size_t callback_count = 0;
    runtime.RegisterTask({
        .task_id = RequireTaskId("task-1"),
        .trigger_at = TriggerAt{2h},
        .callback = [&callback_count](const TaskId&, TriggerAt) { ++callback_count; },
        .on_result = [](RegisterTaskResult) {},
    });
    runtime.ProcessWake();
    const auto notifications_before_expiry = wake.notification_count;
    clock.SetNow(TriggerAt{2h});

    timer.Fire();

    Check(wake.notification_count == notifications_before_expiry + 1, "timer expiry should only notify the Runner");
    Check(callback_count == 0, "timer expiry callback should not execute Timing business callbacks");

    runtime.ProcessWake();

    Check(callback_count == 1, "normal Runner context should execute the due callback after wake");
}

void RefreshesClockAfterCallbacksBeforeArmingNextWake() {
    InMemoryTimingTaskRunner runner;
    SequenceClock clock({TriggerAt{2h}, TriggerAt{4h}});
    RecordingOneShotTimer timer;
    RecordingRunnerWake wake;
    TimingTaskRuntime runtime(runner, clock, timer, wake);
    runtime.RegisterTask(Registration("due", TriggerAt{2h}));
    runtime.RegisterTask(Registration("became-overdue", TriggerAt{3h}));

    runtime.ProcessWake();

    Check(timer.armed_delays == std::vector<std::chrono::microseconds>{0us},
          "Runner should recompute delay from the clock after callbacks finish");
}

void ClearsTimerCallbackBeforeRuntimeDependenciesCanOutliveIt() {
    InMemoryTimingTaskRunner runner;
    FixedClock clock(TriggerAt{1h});
    RecordingOneShotTimer timer;
    RecordingRunnerWake wake;
    { TimingTaskRuntime runtime(runner, clock, timer, wake); }

    timer.FireIfBound();

    Check(timer.clear_count == 1, "runtime destruction should wait while clearing the timer callback");
    Check(wake.notification_count == 0, "destroyed runtime should no longer receive timer expiry notifications");
}

void ResultCallbackRegistrationQueuesAndWakesTheNextRunnerTurn() {
    InMemoryTimingTaskRunner runner;
    FixedClock clock(TriggerAt{1h});
    RecordingOneShotTimer timer;
    RecordingRunnerWake wake;
    TimingTaskRuntime runtime(runner, clock, timer, wake);
    runtime.RegisterTask({
        .task_id = RequireTaskId("first"),
        .trigger_at = TriggerAt{5h},
        .callback = [](const TaskId&, TriggerAt) {},
        .on_result =
            [&runtime](RegisterTaskResult) { runtime.RegisterTask(Registration("from-result", TriggerAt{3h})); },
    });

    runtime.ProcessWake();

    Check(wake.notification_count == 2, "result callback registration should notify a later Runner turn");
    Check(timer.armed_delays == std::vector<std::chrono::microseconds>{4h},
          "current turn should arm from commands applied before its fixed command snapshot");

    runtime.ProcessWake();

    Check(timer.armed_delays == std::vector<std::chrono::microseconds>({4h, 2h}),
          "next Runner turn should apply the command registered by the result callback");
}

void ReportsOneShotTimerArmFailure() {
    InMemoryTimingTaskRunner runner;
    FixedClock clock(TriggerAt{1h});
    RecordingOneShotTimer timer;
    RecordingRunnerWake wake;
    TimingTaskRuntime runtime(runner, clock, timer, wake);
    runtime.RegisterTask(Registration("task", TriggerAt{2h}));
    timer.arm_result = Status::Error(ErrorCode::kUnavailable, "timer busy");

    const auto result = runtime.ProcessWake();
    Check(result.code == ErrorCode::kUnavailable, "Runner should preserve a platform timer arm failure category");
}

}  // namespace

int main() {
    ArmsOnlyTheEarliestPendingTaskAfterRegistrationWake();
    DisarmsTimerWhenNoTaskRemainsPending();
    ClampsOverdueDeferredTaskToZeroDelay();
    TimerExpiryOnlyWakesRunnerUntilNormalContextProcessesIt();
    RefreshesClockAfterCallbacksBeforeArmingNextWake();
    ClearsTimerCallbackBeforeRuntimeDependenciesCanOutliveIt();
    ResultCallbackRegistrationQueuesAndWakesTheNextRunnerTurn();
    ReportsOneShotTimerArmFailure();
    return 0;
}
