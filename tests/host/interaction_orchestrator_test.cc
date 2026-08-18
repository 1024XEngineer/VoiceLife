#include "voicelife/application/interaction_orchestrator.h"

#include <vector>

#include "support/test_support.h"

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
    const InteractionOrchestrator orchestrator;
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
        orchestrator.Handle(event, first_trace);
        orchestrator.Handle(event, second_trace);
    }

    Check(first_trace.trace == expected_trace, "编排器必须为固定事件序列生成预期动作轨迹");
    Check(second_trace.trace == first_trace.trace, "相同事件序列必须生成相同动作轨迹");
    return 0;
}
