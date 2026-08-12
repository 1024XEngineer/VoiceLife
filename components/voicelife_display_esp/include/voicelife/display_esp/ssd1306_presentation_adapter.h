#pragma once

#include "voicelife/contracts/status.h"
#include "voicelife/voice/voice_ports.h"

namespace voicelife::display_esp {

/**
 * @brief VoiceLife PCB 128x32 SSD1306 点阵屏显示适配器。
 *
 * 包装现有 InitializeStatusDisplay/SetEmotion 自由函数（不改动旧实现），
 * 把 DisplaySnapshot 业务语义映射为旧点阵界面（牛头表情 + 状态栏 + 内容
 * 栏）。文本渲染与滚动属于本 Adapter 的专属上下文；点阵屏无图片/动画
 * 能力，Submit 一律返回 kUnavailable。旧 VoiceLife PCB 通过
 * VoiceLifePcbAssembly 暴露本 Adapter，Runtime 不直接调用 SetEmotion。
 */
class Ssd1306PresentationAdapter : public voicelife::voice::PresentationPort {
   public:
    /** @brief 构造函数。 */
    Ssd1306PresentationAdapter() = default;
    /** @brief 虚析构函数。 */
    ~Ssd1306PresentationAdapter() override = default;

    /**
     * @brief 返回点阵屏能力声明：文本可用，无图片/动画能力。
     * @return 显示能力引用。
     */
    [[nodiscard]] const voicelife::voice::DisplayCapabilities& capabilities() const override;

    /**
     * @brief 将显示快照映射为点阵屏文本界面并提交给旧渲染实现。
     *
     * 宿主（host）构建下不触碰硬件，仅作为契约测试路径返回成功。
     * @param snapshot 只包含业务语义的显示快照。
     * @return 渲染结果。
     */
    voicelife::Status Render(const voicelife::voice::DisplaySnapshot& snapshot) override;

    /**
     * @brief 提交受资源清单约束的显示命令。
     *
     * 点阵屏不支持图片/动画资源，一律返回 kUnavailable。
     * @param command 逻辑资源命令。
     * @return 恒为 kUnavailable（能力不支持）。
     */
    voicelife::Status Submit(voicelife::voice::PresentationCommand command) override;
};

}  // namespace voicelife::display_esp
