#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>

#include "voicelife/voice/voice_ports.h"

namespace voicelife::voice {

/** 本地唤醒检测器的最小端口，不暴露平台 DSP 类型。 */
class LocalWakeDetectorPort {
   public:
    using WakeSink = std::function<void(std::string_view wake_word)>;

    /** @brief 释放检测器资源。 */
    virtual ~LocalWakeDetectorPort() = default;
    /** @brief 启动本地检测，并在命中命令时调用回调。
     * @param sink 唤醒命令回调。
     * @return 检测器启动成功时返回成功。
     */
    virtual Status Start(WakeSink sink) = 0;
    /** @brief 停止本地检测，不再产生唤醒回调。
     * @return 检测器停止成功时返回成功。
     */
    virtual Status Stop() = 0;
    /** @brief 提交一帧待机 PCM 给本地检测器。
     * @param frame 采集到的待机音频帧。
     * @return 检测器接受该帧时返回成功。
     */
    virtual Status Submit(AudioFrame frame) = 0;
};

/**
 * 将一个物理输入端口复用为本地待机检测与云端上行两种状态。
 *
 * 待机时 PCM 只交给检测器；会话开始采集后才交给 VoiceSession。
 * 检测回调由调用方转交控制任务，不能在回调内同步进行网络操作。
 */
class WakeGateAudioInput final : public AudioInputPort {
   public:
    using WakeSink = LocalWakeDetectorPort::WakeSink;

    /** @brief 使用同一个物理输入创建本地待机和上行采集门控。
     * @param physical_input 唯一的板载物理输入端口。
     * @param detector 本地唤醒检测器。
     * @param local_wake_enabled 是否启用本地待机唤醒检测。
     */
    WakeGateAudioInput(AudioInputPort& physical_input, LocalWakeDetectorPort& detector, bool local_wake_enabled = true);

    /** @brief 设置唤醒命中后由控制层处理的回调。
     * @param sink 由控制任务消费的唤醒回调。
     */
    void SetWakeSink(WakeSink sink);
    /** @brief 在已经 Open() 后进入本地待机采集。
     * @return 检测器和物理采集均就绪时返回成功。
     */
    Status StartStandby();
    /**
     * @brief 暂时丢弃本地唤醒检测帧，但保持物理采集可用。
     *
     * 终结型 TTS 可能包含本地命令词；无 AEC 板型必须在其后留出余响窗口。
     * @param duration_ms 丢弃本地唤醒检测帧的持续时间（毫秒）。
     */
    void SuppressLocalWakeFor(uint32_t duration_ms);
    /** @brief 查询当前是否只运行本地待机检测。
     * @return 处于待机且没有向云端转发 PCM 时返回 true。
     */
    [[nodiscard]] bool standby() const;

    /** @brief 设置会话采集状态下的 PCM 接收回调。
     * @param sink 云端上行状态的 PCM 回调。
     */
    void SetAudioSink(AudioFrameSink sink) override;
    /** @brief 打开底层物理输入并绑定门控回调。
     * @param format 物理输入必须使用的 PCM 格式。
     * @return 物理输入可用于待机或上行采集时返回成功。
     */
    Status Open(const AudioFormat& format) override;
    /** @brief 从本地待机切换为向会话上行 PCM。
     * @param mode 本轮会话采集模式。
     * @return 切换成功时返回成功。
     */
    Status StartCapture(VoiceMode mode) override;
    /** @brief 停止向会话上行，但不隐式恢复本地待机检测。
     *
     * 播报开始和最终 STT 等待都需要停止云端上行；在这些中间状态重新打开
     * 本地命令检测会把扬声器回声识别为唤醒词。只有 Runtime 在交互状态
     * 明确回到 Standby 后才调用 StartStandby() 恢复检测器。
     * @return 上行已停止时返回成功。
     */
    Status StopCapture() override;
    /** @brief 停止检测和物理采集，并释放输入端口。 */
    void Close() override;

   private:
    Status StartDetectorLocked();
    Status StopDetectorLocked();
    void HandlePhysicalFrame(AudioFrame frame);
    void HandleWakeWord(std::string_view wake_word);

    AudioInputPort& physical_input_;
    LocalWakeDetectorPort& detector_;
    bool local_wake_enabled_ = true;
    mutable std::mutex mutex_;
    AudioFrameSink audio_sink_;
    WakeSink wake_sink_;
    bool opened_ = false;
    bool physical_running_ = false;
    bool detector_running_ = false;
    bool forwarding_ = false;
    std::chrono::steady_clock::time_point wake_suppressed_until_{};
};

}  // namespace voicelife::voice
