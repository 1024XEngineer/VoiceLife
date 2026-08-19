#pragma once

#include <cstdint>
#include <functional>
#include <string_view>

#include "voicelife/voice/voice_ports.h"
#include "voicelife/voice/wake_gate_audio_input.h"

namespace voicelife::runtime {

/** @brief 板级输入适配器向 Runtime 投递的纯交互语义。 */
enum class BoardInputAction : uint8_t {
    kToggleChat,
    kPressDown,
    kPressUp,
    kVolumeUp,
    kVolumeDown,
    kVolumeMaximum,
    kVolumeMute,
    /** Physical long-press request: reboot into local SoftAP Wi-Fi provisioning. */
    kStartWifiProvisioning,
};

/** @brief Runtime 提供给板级输入适配器的非阻塞语义事件入口。 */
using BoardInputSink = std::function<void(BoardInputAction)>;

/**
 * @brief 构建期选定的平台装配组合根。
 *
 * 每个受支持板型（VoiceLife PCB / ESP-SparkBot）通过各自的 Assembly 暴露
 * 稳定的平台 Port 与板级事实（显示、音频、语义输入、共享电源）。
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
     * @brief 返回板级音频采集端口（业务 PCM 语义，不暴露 I2S/Codec）。
     * @return 音频输入端口引用。
     */
    virtual voicelife::voice::AudioInputPort& audio_input() = 0;

    /**
     * @brief 返回板级音频播放端口（业务 PCM 语义）。
     * @return 音频输出端口引用。
     */
    virtual voicelife::voice::AudioOutputPort& audio_output() = 0;

    /**
     * @brief 返回板级唤醒门控（含具体唤醒检测器，Assembly 持有）。
     * @return 唤醒门控引用。
     */
    virtual voicelife::voice::WakeGateAudioInput& wake_gate() = 0;

    /**
     * @brief 返回可选的测试 PCM 注入端口。
     *
     * 仅测试 Profile 的串口回归工具使用；正常产品路径不得依赖它。
     * @return 平台支持时返回注入端口，否则返回空指针。
     */
    virtual voicelife::voice::TestAudioInjectionPort* test_audio_injection() { return nullptr; }

    /**
     * @brief 是否在空闲态运行板载本地唤醒模型。
     *
     * 没有 ESP-SR model 分区的板型仍可经 BOOT 键进入云端采集；Runtime 依赖
     * 此能力而非板型名称决定是否重启本地检测器。
     * @return 启用本地唤醒检测时返回 true。
     */
    virtual bool uses_local_wake_detector() const { return true; }

    /**
     * @brief 返回构建期板型身份（OTA/策略请求用，如 esp-sparkbot）。
     * @return 板型标识。
     */
    virtual std::string_view board_identity() const = 0;

    /**
     * @brief 初始化板级 LED（构建期板型专属；无 LED 板为空实现）。
     */
    virtual void InitializeBoardLeds() {}

    /**
     * @brief 设置输出音量（0-100）。默认空实现。
     * @param volume 音量值。
     */
    virtual void SetOutputVolume(uint8_t /*volume*/) {}

    /**
     * @brief 打印板级音频统计日志。默认空实现。
     */
    virtual void LogAudioStats() {}

    /**
     * @brief 启动板级输入适配器。
     *
     * Assembly 负责 GPIO、触摸、去抖和物理按键到 BoardInputAction 的映射；
     * Runtime 只接收语义事件并投递到 InteractionEventLoop。无输入设备的板型
     * 保持默认成功，不能把 GPIO 或板型分支泄漏到 Runtime。
     * @param sink 非阻塞的板级输入语义事件入口。
     * @return 启动结果。
     */
    virtual voicelife::Status StartBoardInput(BoardInputSink /*sink*/) { return voicelife::Status::Ok(); }

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
