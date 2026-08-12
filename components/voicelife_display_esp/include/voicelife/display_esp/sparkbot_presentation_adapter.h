#pragma once

#include <string>
#include <string_view>

#include "voicelife/contracts/status.h"
#include "voicelife/voice/voice_ports.h"

namespace voicelife::display_esp {

/**
 * @brief 官方 SparkBot 牛头表情资源标识（与 assets 资源清单 manifest.json 一致）。
 *
 * 这是显示层专属的受控值对象：Voice/domain 只允许携带这些标识，不允许
 * 构造文件路径、URL 或任意字符串进入显示端口。
 */
enum class SparkBotAssetId {
    kBoot,
    kConnecting,
    kError,
    kHappy,
    kIdle,
    kListening,
    kProvisioning,
    kSleepy,
    kSpeaking,
    kThinking,
};

/**
 * @brief 解析受控资源标识。
 *
 * 空值、含路径分隔符（/、\\）、.. 或绝对路径特征视为非法格式；
 * 格式合法但不在官方资源清单中的标识返回 kNotFound。
 * @param asset_id 调用方提供的资源标识。
 * @return 解析成功返回对应的受控枚举。
 */
[[nodiscard]] voicelife::Result<SparkBotAssetId> ParseSparkBotAssetId(std::string_view asset_id);

/**
 * @brief ESP-SparkBot 彩屏显示适配器（架构骨架）。
 *
 * 通过 voice::PresentationPort 消费 DisplaySnapshot 和受资源清单约束的
 * PresentationCommand，把业务语义映射到板级 ST7789/LVGL 渲染。LVGL 对象、
 * GIF 解码、图片缓存、像素缓冲区和刷新任务只能存在于本适配器的专属上下
 * 文；Provider 回调、音频实时任务、输入源和定时器不得直接刷屏（唯一提交
 * 者是交互事件循环，渲染只在本 Adapter 专属显示任务中执行）。
 *
 * 本骨架尚未移植官方 Renderer：capabilities().available 为 false 且所有可
 * 运行能力（text/static_image/animation/preview_image）均为 false，不伪装
 * 成已经支持。Render 返回 kUnavailable；Submit 对非法格式返回
 * kInvalidArgument、对未知资源返回 kNotFound、对受控资源返回 kUnavailable。
 *
 * 移植时唯一实现来源是小智官方 SparkBot 显示实现（不得自行重新设计布局、
 * 主题、字体或动画）：
 *   - xiaozhi-esp32 commit 37d1aee793f99a9b865957acc3798d06335c3ad0
 *   - main/boards/espressif/esp-sparkbot/
 *   - main/display/lcd_display.cc
 *   - main/display/lvgl_display/
 *
 * 资源只允许通过 manifest 约束的 asset_id 使用，不接受网络 URL、任意文件
 * 路径或未验证字节流。
 */
class SparkBotPresentationAdapter : public voicelife::voice::PresentationPort {
   public:
    /** @brief 构造函数。 */
    SparkBotPresentationAdapter() = default;
    /** @brief 虚析构函数。 */
    ~SparkBotPresentationAdapter() override = default;

    /**
     * @brief 返回 SparkBot 彩屏能力声明。
     *
     * 官方 Renderer 未移植前 available 为 false，text/static_image/
     * animation/preview_image 全部为 false；max_frame_bytes 仅表示
     * 240x240 RGB565 单帧缓冲的硬件上限，Runtime 不得据此分配资源。
     * @return 显示能力引用。
     */
    [[nodiscard]] const voicelife::voice::DisplayCapabilities& capabilities() const override;

    /**
     * @brief 提交一份完整显示快照。
     *
     * 骨架阶段不执行任何渲染，返回 kUnavailable。
     * @param snapshot 只包含业务语义的显示快照。
     * @return 恒为 kUnavailable（Renderer 未移植）。
     */
    voicelife::Status Render(const voicelife::voice::DisplaySnapshot& snapshot) override;

    /**
     * @brief 提交受资源清单约束的显示命令。
     *
     * 只接受受控 asset_id：非法格式（空/路径分隔符/..）返回
     * kInvalidArgument，未知资源返回 kNotFound，受控资源返回
     * kUnavailable（Renderer 未移植）。本骨架不接触文件系统、网络或
     * 解码器。
     * @param command 逻辑资源 ID、代次和幂等请求 ID。
     * @return 按上述契约返回明确状态。
     */
    voicelife::Status Submit(voicelife::voice::PresentationCommand command) override;
};

}  // namespace voicelife::display_esp
