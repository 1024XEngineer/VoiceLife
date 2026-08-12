#pragma once

#include <vector>

#include "voicelife/audio_esp/audio_board_profile.h"
#include "voicelife/voice/voice_ports.h"

namespace voicelife::runtime {

/**
 * @brief 构建期选定的平台装配组合根。
 *
 * 每个受支持板型（VoiceLife PCB / ESP-SparkBot）通过各自的 Assembly 暴露
 * 稳定的平台 Port 与板级事实（显示、音频 Profile、按键 GPIO、共享电源）。
 * Runtime 只依赖本接口：不判断板型、不引用具体 Adapter 或图形框架对象；
 * 显示快照的生产（Domain/交互事件循环）与消费（显示 Adapter 专属上下文）
 * 据此解耦。
 *
 * 本接口是架构骨架：真实硬件拓扑的其余 Port（输入、唤醒、连接）与
 * 构建期 Profile 描述符由后续 MS-A/MS-B 扩展。
 */
class PlatformAssembly {
   public:
    /** @brief 虚析构函数。 */
    virtual ~PlatformAssembly() = default;

    /**
     * @brief 返回板型选定的显示端口。
     *
     * 调用方必须遵守 PresentationPort 的调用上下文契约（仅显示任务/受控
     * 上下文提交，唯一提交者为交互事件循环）。
     * @return 显示端口引用。
     */
    virtual voicelife::voice::PresentationPort& presentation() = 0;

    /**
     * @brief 启动板级资源（如显示初始化）。
     *
     * 默认空实现；需要真实硬件初始化的 Assembly 覆写。host 构建不触碰
     * 硬件，应返回明确状态而不是伪装成功。
     * @return 启动结果。
     */
    virtual voicelife::Status Start() { return voicelife::Status::Ok(); }

    /**
     * @brief 返回构建期选定的音频板 Profile（I2S 端点与 Codec 控制）。
     * @return 音频 Profile（按值，Assembly 内持有来源）。
     */
    virtual audio_esp::AudioBoardProfile audio_profile() const = 0;

    /**
     * @brief 返回板级按键 GPIO 列表。
     *
     * 顺序语义：boot / touch / volume_up / volume_down；无按键的板型返回
     * 空列表（Runtime 不启动按键任务）。SPI/音频复用引脚不得出现在列表。
     * @return 按键 GPIO 列表。
     */
    virtual std::vector<int> button_gpios() const { return {}; }

    /**
     * @brief 请求更新音频输出（功放）使能，经板级统一仲裁。
     *
     * 默认空实现；需要 GPIO46 功放仲裁的板型覆写。音频模块只提交请求，
     * 不得直接写 GPIO。
     * @param enabled 是否请求音频输出启用。
     * @return 仲裁请求结果。
     */
    virtual voicelife::Status SetAudioOutputEnabled(bool /*enabled*/) { return voicelife::Status::Ok(); }
};

}  // namespace voicelife::runtime
