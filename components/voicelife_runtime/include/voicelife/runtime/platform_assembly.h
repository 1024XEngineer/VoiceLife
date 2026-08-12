#pragma once

#include "voicelife/voice/voice_ports.h"

namespace voicelife::runtime {

/**
 * @brief 构建期选定的平台装配组合根。
 *
 * 每个受支持板型（VoiceLife PCB / ESP-SparkBot）通过各自的 Assembly 暴露
 * 稳定的平台 Port。Runtime 只依赖本接口：不判断板型、不引用具体 Adapter
 * 或图形框架对象；显示快照的生产（Domain/交互事件循环）与消费（显示
 * Adapter 专属上下文）据此解耦。
 *
 * 本接口是架构骨架：真实硬件拓扑的其余 Port（音频、输入、唤醒、连接）
 * 与构建期 Profile 描述符由后续 MS-A/MS-B 扩展。
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
};

}  // namespace voicelife::runtime
