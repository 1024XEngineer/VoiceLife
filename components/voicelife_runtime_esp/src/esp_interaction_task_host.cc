#include "voicelife/runtime_esp/esp_interaction_task_host.h"

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#endif

namespace voicelife::runtime_esp {

EspInteractionTaskHost::EspInteractionTaskHost(application::InteractionOrchestrator& orchestrator)
    : orchestrator_(orchestrator) {}

void EspInteractionTaskHost::Submit(application::InteractionEvent event,
                                    application::InteractionActionSink& actions) const {
#ifdef ESP_PLATFORM
    static_assert(configMAX_PRIORITIES > 0, "FreeRTOS task priorities must be configured");
#endif
    orchestrator_.Handle(event, actions);
}

}  // namespace voicelife::runtime_esp
