#pragma once

#include "voicelife/application/interaction_orchestrator.h"

namespace voicelife::runtime_esp {

/**
 * @brief ESP 侧交互任务的窄适配器。
 *
 * 本骨架只建立 Runtime Adapter 到应用服务的调用路径，未创建 FreeRTOS task，
 * 也未接管既有 Runtime 事件循环。后续迁移只能从该适配器进入。
 */
class EspInteractionTaskHost {
   public:
    /** @brief 创建使用指定应用服务的 ESP 交互任务宿主。 @param orchestrator 生命周期覆盖宿主的编排器。 */
    explicit EspInteractionTaskHost(application::InteractionOrchestrator& orchestrator);

    /**
     * @brief 将 ESP 侧已归一化的事件交给平台无关的编排器。
     * @param event 已归一化的交互事件。
     * @param actions 用于接收编排动作的 Runtime Adapter 端口。
     */
    void Submit(application::InteractionEvent event, application::InteractionActionSink& actions) const;

   private:
    application::InteractionOrchestrator& orchestrator_;
};

}  // namespace voicelife::runtime_esp
