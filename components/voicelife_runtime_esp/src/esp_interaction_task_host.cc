#include "voicelife/runtime_esp/esp_interaction_task_host.h"

#include "freertos/FreeRTOS.h"

namespace voicelife::runtime_esp {

EspInteractionTaskHost::EspInteractionTaskHost(application::InteractionOrchestrator& orchestrator)
    : orchestrator_(orchestrator) {}

Status EspInteractionTaskHost::Submit(application::InteractionEvent event,
                                      application::InteractionActionSink& actions) {
    static_assert(configMAX_PRIORITIES > 0, "FreeRTOS task priorities must be configured");
    return orchestrator_.Handle(event, actions);
}

}  // namespace voicelife::runtime_esp
