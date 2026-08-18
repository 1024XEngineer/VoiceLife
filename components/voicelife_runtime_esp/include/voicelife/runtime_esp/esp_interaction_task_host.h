#pragma once

#include "voicelife/application/interaction_orchestrator.h"

namespace voicelife::runtime_esp {

/**
 * @brief ESP 侧交互任务的窄适配器。
 *
 * 它只负责将既有 FreeRTOS 事件循环归一化的事件送到应用服务；任务、队列和
 * 定时器的所有权仍在 Runtime ESP Adapter。
 */
class EspInteractionTaskHost {
   public:
    /** @brief 创建使用指定应用服务的 ESP 交互任务宿主。 @param orchestrator 平台无关的交互编排器。 */
    explicit EspInteractionTaskHost(application::InteractionOrchestrator& orchestrator);

    /**
     * @brief 将 ESP 侧已归一化的事件交给平台无关的编排器。
     * @param event 已归一化的交互事件。
     * @param actions 用于接收编排动作的 Runtime Adapter 端口。
     * @return 事件编排和动作投影结果。
     */
    Status Submit(application::InteractionEvent event, application::InteractionActionSink& actions);

   private:
    application::InteractionOrchestrator& orchestrator_;
};

}  // namespace voicelife::runtime_esp
