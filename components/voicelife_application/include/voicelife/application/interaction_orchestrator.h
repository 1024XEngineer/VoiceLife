#pragma once

#include <string>
#include <string_view>

#include "voicelife/contracts/status.h"
#include "voicelife/voice/voice_interaction_controller.h"

namespace voicelife::application {

/** @brief 一次交互编排请求的稳定输入模型，不携带 ESP-IDF 或 FreeRTOS 类型。 */
struct InteractionEvent {
    voice::VoiceInteractionEvent voice_event = voice::VoiceInteractionEvent::kBootCompleted;
    std::string_view wake_word;
};

/** @brief 一次合法状态迁移产生的、可由平台适配器执行的语义动作。 */
struct InteractionAction {
    voice::VoiceInteractionEvent source = voice::VoiceInteractionEvent::kBootCompleted;
    voice::VoiceInteractionState state = voice::VoiceInteractionState::kBooting;
    voice::VoiceInteractionAction directive = voice::VoiceInteractionAction::kNone;
    std::string wake_word;

    friend bool operator==(const InteractionAction& lhs, const InteractionAction& rhs) {
        return lhs.source == rhs.source && lhs.state == rhs.state && lhs.directive == rhs.directive &&
               lhs.wake_word == rhs.wake_word;
    }
};

/** @brief Runtime Adapter 实现的动作接收端口。 */
class InteractionActionSink {
   public:
    /** @brief 虚析构函数。 */
    virtual ~InteractionActionSink() = default;
    /**
     * @brief 接收一个应用层编排动作。
     * @param action 要执行的语义动作。
     * @return 动作投影结果。
     */
    virtual Status Submit(InteractionAction action) = 0;
};

/**
 * @brief 平台无关的交互应用服务。
 *
 * 该服务拥有平台无关的交互状态机，并将合法状态迁移交给 Runtime Adapter
 * 执行。FreeRTOS 队列、任务和定时器仍属于 ESP Runtime Adapter。
 */
class InteractionOrchestrator {
   public:
    /**
     * @brief 将一个交互事件映射为状态和确定的 Runtime Adapter 动作。
     * @param event 要编排的跨域交互事件。
     * @param actions 用于记录或执行动作的 Runtime Adapter 端口。
     * @return 状态机接受事件且动作投影成功时返回成功状态。
     */
    Status Handle(InteractionEvent event, InteractionActionSink& actions);

    /** @brief 返回最后一个已接受事件后的交互状态。 @return 当前交互状态。 */
    [[nodiscard]] voice::VoiceInteractionState state() const;

   private:
    voice::VoiceInteractionController controller_;
};

}  // namespace voicelife::application
