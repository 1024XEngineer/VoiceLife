#include "voicelife/timing/timing_runtime.h"

#include <algorithm>
#include <utility>

namespace voicelife::timing {

TimingTaskRuntime::TimingTaskRuntime(InMemoryTimingTaskRunner& runner, ClockPort& clock, OneShotTimerPort& timer,
                                     RunnerWakePort& wake)
    : runner_(runner), clock_(clock), timer_(timer), wake_(wake) {
    timer_.SetExpiryCallback([this] { wake_.Notify(); });
}

TimingTaskRuntime::~TimingTaskRuntime() { timer_.ClearExpiryCallbackAndWait(); }

CommandAcceptance TimingTaskRuntime::RegisterTask(RegisterTaskCommand command) {
    const auto callback_internal = runner_.IsInCallbackContext();
    const auto acceptance = runner_.RegisterTask(std::move(command));
    if (!callback_internal) {
        wake_.Notify();
    }
    return acceptance;
}

CommandAcceptance TimingTaskRuntime::CancelTask(CancelTaskCommand command) {
    const auto callback_internal = runner_.IsInCallbackContext();
    const auto acceptance = runner_.CancelTask(std::move(command));
    if (!callback_internal) {
        wake_.Notify();
    }
    return acceptance;
}

Status TimingTaskRuntime::ProcessWake() {
    const auto now = clock_.Now();
    const auto result = runner_.RunDueTasks(now);
    if (!result.next_wake_at.has_value()) {
        return timer_.Disarm();
    }
    const auto arm_now = clock_.Now();
    return timer_.ArmAfter(std::max(std::chrono::microseconds::zero(), *result.next_wake_at - arm_now));
}

}  // namespace voicelife::timing
