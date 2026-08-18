#include "voicelife/application/interaction_orchestrator.h"

namespace voicelife::application {

Status InteractionOrchestrator::Handle(InteractionEvent event, InteractionActionSink& actions) {
    const auto transition = controller_.Handle(event.voice_event);
    if (!transition.ok() || !transition.value.has_value()) {
        return transition.status;
    }
    return actions.Submit(
        {.source = event.voice_event, .state = transition.value->state, .directive = transition.value->action});
}

voice::VoiceInteractionState InteractionOrchestrator::state() const { return controller_.state(); }

}  // namespace voicelife::application
