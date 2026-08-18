#include "voicelife/application/interaction_orchestrator.h"

#include <type_traits>
#include <vector>

#include "support/test_support.h"
#include "voicelife/runtime_esp/esp_interaction_task_host.h"

namespace {

using voicelife::application::InteractionAction;
using voicelife::application::InteractionActionKind;
using voicelife::application::InteractionActionSink;
using voicelife::application::InteractionEvent;
using voicelife::application::InteractionEventKind;
using voicelife::application::InteractionOrchestrator;
using voicelife::test::Check;

class TraceSink final : public InteractionActionSink {
   public:
    void Submit(InteractionAction action) override { trace.push_back(action); }

    std::vector<InteractionAction> trace;
};

}  // namespace

int main() {
    static_assert(std::is_constructible_v<voicelife::runtime_esp::EspInteractionTaskHost, InteractionOrchestrator&>);
    static_assert(!std::is_constructible_v<voicelife::runtime_esp::EspInteractionTaskHost, InteractionOrchestrator&&>);
    InteractionOrchestrator orchestrator;
    const voicelife::runtime_esp::EspInteractionTaskHost host(orchestrator);
    const std::vector<InteractionEvent> events = {
        {.kind = InteractionEventKind::kBootstrapRequested},
        {.kind = InteractionEventKind::kBoardInputArrived},
        {.kind = InteractionEventKind::kVoiceLifecycleChanged},
        {.kind = InteractionEventKind::kConnectivityChanged},
    };
    const std::vector<InteractionAction> expected_trace = {
        {.kind = InteractionActionKind::kInitializeInteraction},
        {.kind = InteractionActionKind::kDispatchBoardInput},
        {.kind = InteractionActionKind::kDispatchVoiceLifecycle},
        {.kind = InteractionActionKind::kRefreshConnectivity},
    };

    TraceSink first_trace;
    TraceSink second_trace;
    for (const InteractionEvent event : events) {
        host.Submit(event, first_trace);
        host.Submit(event, second_trace);
    }

    Check(first_trace.trace == expected_trace, "编排器必须为固定事件序列生成预期动作轨迹");
    Check(second_trace.trace == first_trace.trace, "相同事件序列必须生成相同动作轨迹");
    return 0;
}
