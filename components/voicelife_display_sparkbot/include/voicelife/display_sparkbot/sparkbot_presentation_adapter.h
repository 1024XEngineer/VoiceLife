#pragma once

#include <cstddef>
#include <functional>

#include "voicelife/contracts/status.h"
#include "voicelife/display_sparkbot/display_snapshot_queue.h"
#include "voicelife/display_sparkbot/sparkbot_lvgl_display.h"
#include "voicelife/display_sparkbot/sparkbot_lvgl_renderer.h"
#include "voicelife/voice/voice_ports.h"

namespace voicelife::display_sparkbot {

/**
 * @brief ESP-SparkBot 彩屏显示适配器（完整显示链路）。
 *
 * 调用链：PresentationPort.Render -> 有界队列 -> 专属显示任务（LVGL 锁内）
 * -> SparkBotLvglRenderer -> 官方 ST7789/LVGL 显示。
 *
 * 约束：
 * - LVGL 对象、GIF 解码、缓存、像素缓冲与刷新只在本 Adapter 的专属显示
 *   任务/LVGL 锁上下文执行；Provider 回调、音频实时任务、输入和定时器
 *   不得直接调用 Renderer。
 * - 显示任务消费快照时丢弃旧 revision（小于等于已渲染 revision 的迟到
 *   快照），避免旧状态覆盖新状态。
 * - 队列有界（容量固定），满时丢弃最旧快照，显示阻塞不反向阻塞语音。
 * - 资源只接受受控 asset_id；提交命令不与 Renderer 直连。
 *
 * 实板显示未验证：available=true 表示显示链路（代码级）闭合，不代表
 * 官方 SparkBot 显示已在实板验收。
 */
class SparkBotPresentationAdapter : public voicelife::voice::PresentationPort {
   public:
    /** @brief 背光回调（板级仲裁入口，待机时关闭）。 */
    using BacklightCallback = std::function<void(bool)>;

    /**
     * @brief 构造函数。
     * @param config 官方 SparkBot 板级显示参数。
     * @param backlight 可选背光回调（经板级仲裁更新 GPIO46；不传则不控制）。
     */
    explicit SparkBotPresentationAdapter(const SparkBotLcdConfig& config, BacklightCallback backlight = {});

    /** @brief 虚析构函数：停止显示任务并释放资源。 */
    ~SparkBotPresentationAdapter() override;

    /** @brief 禁止拷贝构造。 */
    SparkBotPresentationAdapter(const SparkBotPresentationAdapter&) = delete;
    /** @brief 禁止拷贝赋值。 */
    SparkBotPresentationAdapter& operator=(const SparkBotPresentationAdapter&) = delete;

    /**
     * @brief 返回 SparkBot 彩屏能力声明（显示链路已闭合）。
     * @return 显示能力引用。
     */
    [[nodiscard]] const voicelife::voice::DisplayCapabilities& capabilities() const override;

    /**
     * @brief 提交一份完整显示快照（入有界队列）。
     *
     * 立即返回（快照已接受）；渲染由专属显示任务异步执行。
     * @param snapshot 只包含业务语义的显示快照。
     * @return 快照被接受返回 Ok。
     */
    voicelife::Status Render(const voicelife::voice::DisplaySnapshot& snapshot) override;

    /**
     * @brief 提交受资源清单约束的显示命令。
     *
     * 只接受受控 asset_id；非法格式返回 kInvalidArgument，未知资源返回
     * kNotFound。独立资源命令（如预览图）本阶段未实现，返回 kUnavailable；
     * 动画由 Render 快照的 mood 驱动。
     * @param command 逻辑资源 ID、代次和幂等请求 ID。
     * @return 按契约返回明确状态。
     */
    voicelife::Status Submit(voicelife::voice::PresentationCommand command) override;

    /**
     * @brief 初始化 ST7789/LVGL 并启动专属显示任务。
     * @return 启动结果。
     */
    [[nodiscard]] voicelife::Status Start();

    /**
     * @brief 停止显示任务并释放资源。
     * @return 停止结果。
     */
    [[nodiscard]] voicelife::Status Stop();

   private:
    /** @brief FreeRTOS 显示任务入口（仅 ESP 构建）。 */
    static void DisplayTaskEntry(void* arg);

    /** @brief 显示任务主循环：消费快照、丢弃旧 revision、LVGL 锁内渲染。 */
    void DisplayTaskLoop();

    /** @brief 显示初始化与 ST7789/LVGL 上下文。 */
    SparkBotLvglDisplay display_;
    /** @brief 官方简单模式 Renderer。 */
    SparkBotLvglRenderer renderer_;
    /** @brief 有界快照队列（满时丢最旧）。 */
    DisplaySnapshotQueue queue_;
    /** @brief FreeRTOS 显示任务句柄（仅 ESP 构建）。 */
    [[maybe_unused]] void* task_handle_ = nullptr;
    /** @brief 已渲染的最新 revision（旧 revision 丢弃基准）。 */
    [[maybe_unused]] uint64_t last_rendered_revision_ = 0;
    /** @brief 是否已启动。 */
    [[maybe_unused]] bool started_ = false;
    /** @brief 背光回调（板级仲裁）。 */
    [[maybe_unused]] BacklightCallback backlight_cb_;
    /** @brief 上次背光请求状态。 */
    [[maybe_unused]] bool backlight_on_ = true;

    /** @brief 按快照阶段更新背光（待机关闭）。 */
    void UpdateBacklight(const voicelife::voice::DisplaySnapshot& snapshot);
};

}  // namespace voicelife::display_sparkbot
