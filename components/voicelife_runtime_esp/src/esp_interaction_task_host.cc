#include "voicelife/runtime_esp/esp_interaction_task_host.h"

#include "freertos/FreeRTOS.h"

namespace voicelife::runtime_esp {

EspInteractionTaskHost::EspInteractionTaskHost(const application::InteractionOrchestrator& orchestrator)
    : orchestrator_(orchestrator) {}

void EspInteractionTaskHost::Submit(application::InteractionEvent event,
                                    application::InteractionActionSink& actions) const {
    static_assert(configMAX_PRIORITIES > 0, "FreeRTOS task priorities must be configured");
    orchestrator_.Handle(event, actions);
}

}  // namespace voicelife::runtime_esp
