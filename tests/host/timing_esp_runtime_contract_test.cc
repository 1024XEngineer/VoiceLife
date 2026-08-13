#include <concepts>
#include <memory>
#include <type_traits>

#include "support/test_support.h"
#include "voicelife/timing_esp/esp_timing_runtime.h"

using voicelife::ErrorCode;
using voicelife::test::Check;
using voicelife::timing::TimingTaskService;
using voicelife::timing_esp::EspTimingTaskRuntime;

static_assert(std::derived_from<EspTimingTaskRuntime, TimingTaskService>);
static_assert(std::has_virtual_destructor_v<EspTimingTaskRuntime>);

int main() {
    const auto created = EspTimingTaskRuntime::Create();

    Check(created.status.code == ErrorCode::kUnavailable,
          "Host must expose an explicit unavailable result instead of pretending ESP resources exist");
    Check(!created.value.has_value(), "failed ESP runtime creation must not return a partial runtime");
    return 0;
}
