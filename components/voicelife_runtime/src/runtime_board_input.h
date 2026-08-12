#pragma once

#ifdef ESP_PLATFORM

#include <functional>
#include <memory>

#include "voicelife/voice/voice_interaction_controller.h"

namespace voicelife::runtime {

/** @brief VoiceLife PCB 的临时 GPIO 输入适配器，向 Runtime 输出语义事件。 */
class VoiceLifePcbBoardInput final {
   public:
    /** @brief 处理语音交互状态机事件的回调。 */
    using InteractionSink = std::function<void(voice::VoiceInteractionEvent)>;
    /** @brief 处理目标音量百分比的回调。 */
    using VolumeSink = std::function<void(int)>;

    /** @brief 使用 Runtime 提供的语义回调创建输入适配器。 */
    VoiceLifePcbBoardInput(InteractionSink interaction_sink, VolumeSink volume_sink);
    /** @brief 释放本地按键状态；任务生命周期与设备 Runtime 一致。 */
    ~VoiceLifePcbBoardInput();

    /** @brief 初始化 GPIO 并启动按键轮询任务。 */
    void Start();

   private:
    /** @brief 单个 GPIO 的去抖与长按状态。 */
    struct ButtonSample;

    /** @brief FreeRTOS 任务入口。 */
    static void TaskEntry(void* context);
    /** @brief 轮询四个旧板按钮并派发语义回调。 */
    void Run();

    InteractionSink interaction_sink_;
    VolumeSink volume_sink_;
    std::unique_ptr<ButtonSample[]> buttons_;
};

}  // namespace voicelife::runtime

#endif
