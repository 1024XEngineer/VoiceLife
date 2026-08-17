#pragma once

#include <mutex>
#include <string>

#include "voicelife/contracts/status.h"
#include "voicelife/display_esp/ssd1306_status_display.h"
#include "voicelife/voice/voice_ports.h"

namespace voicelife::display_esp {

/**
 * @brief VoiceLife PCB 128x32 SSD1306 点阵屏显示适配器。
 *
 * 包装现有 InitializeStatusDisplay/SetEmotion 自由函数（不改动旧实现），
 * 把 DisplaySnapshot 业务语义映射为旧点阵界面（牛头表情 + 状态栏 + 内容
 * 栏）。下行长文本滚动（400ms/字符，末尾留 6 字符窗口）在本 Adapter 的
 * 专属上下文（esp_timer 回调 + SetEmotion）执行，保持旧板滚动行为；
 * 点阵屏无图片/动画能力。
 */
class Ssd1306PresentationAdapter : public voicelife::voice::PresentationPort {
   public:
    /** @brief 显示初始化函数类型；用于把硬件启动路径置于可测边界。 */
    using InitializeFunction = voicelife::Status (*)();

    /**
     * @brief 构造函数。
     * @param initialize 底层 SSD1306 初始化函数。
     */
    explicit Ssd1306PresentationAdapter(InitializeFunction initialize = &InitializeStatusDisplay)
        : initialize_(initialize) {}
    /** @brief 析构函数：释放滚动定时器。 */
    ~Ssd1306PresentationAdapter() override;

    /** @brief 禁止拷贝构造。 */
    Ssd1306PresentationAdapter(const Ssd1306PresentationAdapter&) = delete;
    /** @brief 禁止拷贝赋值。 */
    Ssd1306PresentationAdapter& operator=(const Ssd1306PresentationAdapter&) = delete;

    /**
     * @brief 返回点阵屏能力声明：文本可用，无图片/动画能力。
     * @return 显示能力引用。
     */
    [[nodiscard]] const voicelife::voice::DisplayCapabilities& capabilities() const override;

    /**
     * @brief 初始化 SSD1306 面板，使后续 Render 能真正提交像素。
     * @return 底层面板初始化状态。
     */
    voicelife::Status Start();

    /**
     * @brief 将显示快照映射为点阵屏文本界面并提交给旧渲染实现。
     *
     * 超宽下行内容自动启动滚动（ESP 平台）；宿主（host）构建下不触碰
     * 硬件，仅作为契约测试路径返回成功。
     * @param snapshot 只包含业务语义的显示快照。
     * @return 渲染结果。
     */
    voicelife::Status Render(const voicelife::voice::DisplaySnapshot& snapshot) override;

   private:
    /** @brief 重启下行长文本滚动定时器（仅 ESP 构建使用）。 */
    void RestartScrollTimer(const std::string& content);

    /** @brief 滚动定时器回调（仅 ESP 构建使用）。 */
    static void ScrollEntry(void* arg);

    /** @brief 滚动状态互斥（Render 线程与 esp_timer 回调并发保护）。 */
    mutable std::mutex state_mutex_;
    /** @brief 最近一次快照（滚动渲染数据源）。 */
    [[maybe_unused]] voicelife::voice::DisplaySnapshot last_snapshot_;
    /** @brief 滚动定时器句柄（仅 ESP 构建使用）。 */
    [[maybe_unused]] void* scroll_timer_ = nullptr;
    /** @brief 滚动窗口起始字符。 */
    [[maybe_unused]] std::size_t scroll_offset_ = 0;
    /** @brief 受控的底层面板初始化入口。 */
    InitializeFunction initialize_;
};

}  // namespace voicelife::display_esp
