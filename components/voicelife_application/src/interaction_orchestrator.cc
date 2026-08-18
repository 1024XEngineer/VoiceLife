#include "voicelife/application/interaction_orchestrator.h"

namespace voicelife::application {

void InteractionOrchestrator::Handle(InteractionEvent event, InteractionActionSink& actions) const {
    InteractionActionKind action = InteractionActionKind::kInitializeInteraction;
    switch (event.kind) {
        case InteractionEventKind::kBootstrapRequested:
            action = InteractionActionKind::kInitializeInteraction;
            break;
        case InteractionEventKind::kBoardInputArrived:
            action = InteractionActionKind::kDispatchBoardInput;
            break;
        case InteractionEventKind::kVoiceLifecycleChanged:
            action = InteractionActionKind::kDispatchVoiceLifecycle;
            break;
        case InteractionEventKind::kConnectivityChanged:
            action = InteractionActionKind::kRefreshConnectivity;
            break;
    }
    actions.Submit({.kind = action});
}

}  // namespace voicelife::application
