#pragma once

#include <memory>

#include "voicelife/voice/wake_gate_audio_input.h"

namespace voicelife::audio_esp {

/** @brief ESP-SR MultiNet7 的本地唤醒和语音打断命令检测适配器。 */
class EspMultiNetWakeDetector final : public voice::LocalWakeDetectorPort {
   public:
    /** @brief 创建尚未加载模型的检测器。 */
    EspMultiNetWakeDetector();
    /** @brief 释放 MultiNet 模型和检测资源。 */
    ~EspMultiNetWakeDetector() override;

    /** @brief 检测器禁止复制，避免重复持有模型资源。 */
    EspMultiNetWakeDetector(const EspMultiNetWakeDetector&) = delete;
    /** @brief 检测器禁止赋值，避免重复持有模型资源。 */
    EspMultiNetWakeDetector& operator=(const EspMultiNetWakeDetector&) = delete;

    /** @brief 启动检测并注册“你好牛牛”“牛来”“别说了”三个本地命令。
     * @param sink 命中唤醒词后的回调。
     * @return 模型和命令初始化成功时返回成功。
     */
    Status Start(WakeSink sink) override;
    /** @brief 停止检测并清空当前识别上下文。
     * @return 清理成功时返回成功。
     */
    Status Stop() override;
    /** @brief 提交一帧 16 kHz S16LE 单声道 PCM。
     * @param frame 待机状态下的音频帧。
     * @return 帧格式和模型均可接受时返回成功。
     */
    Status Submit(voice::AudioFrame frame) override;

   private:
    /** @brief 隐藏 ESP-SR 平台对象，避免平台类型进入公共接口。 */
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace voicelife::audio_esp
