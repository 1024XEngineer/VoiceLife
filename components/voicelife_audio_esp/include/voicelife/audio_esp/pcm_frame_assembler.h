#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

#include "voicelife/contracts/status.h"
#include "voicelife/voice/voice_types.h"

namespace voicelife::audio_esp {

/**
 * @brief 将固定硬件 period 组装为 Provider 协商的传输帧。
 *
 * 硬件 period 刻意不属于 AudioFormat：它是板级调度细节，
 * 而 AudioFormat 是传输契约。
 */
class PcmFrameAssembler final {
   public:
    /** @brief 完整帧回调。 */
    using Sink = std::function<Status(voice::AudioFrame)>;

    /**
     * @brief 构造帧组装器。
     * @param frame_format 目标传输帧格式。
     * @param hardware_period_ms 硬件 period 时长（毫秒）。
     */
    PcmFrameAssembler(voice::AudioFormat frame_format, uint16_t hardware_period_ms);
    /** @brief 释放启动期申请的 PCM 缓冲。 */
    ~PcmFrameAssembler();
    /** @brief 禁止复制，避免重复释放预分配 PCM 缓冲。 */
    PcmFrameAssembler(const PcmFrameAssembler&) = delete;
    /** @brief 禁止复制赋值，避免重复释放预分配 PCM 缓冲。 */
    PcmFrameAssembler& operator=(const PcmFrameAssembler&) = delete;

    /** @brief 校验帧格式与 period 参数是否合法。 @return 合法返回 Ok。 */
    [[nodiscard]] Status Validate() const;

    /**
     * @brief 校验并为一个完整传输帧预留有界缓存。
     *
     * 必须在采集任务启动前调用。该步骤会把内存分配从实时 Push 路径移出，
     * 并将分配失败转换为状态码而不是异常。
     * @return 准备成功返回 Ok。
     */
    Status Prepare();

    /** @brief 目标传输帧格式。 @return 帧格式引用。 */
    [[nodiscard]] const voice::AudioFormat& frame_format() const { return frame_format_; }

    /** @brief 每个完整帧包含的样本数。 @return 样本数。 */
    [[nodiscard]] std::size_t frame_samples() const { return frame_samples_; }

    /** @brief 当前待组装的样本数。 @return 挂起样本数。 */
    [[nodiscard]] std::size_t pending_samples() const { return pending_size_; }
    /** @brief 当前上行 pool 的峰值已占用槽位。 */
    [[nodiscard]] std::size_t payload_pool_high_watermark() const;
    /** @brief 当前上行 pool 未能即时获取 slot 的次数。 */
    [[nodiscard]] std::size_t payload_pool_acquisition_failures() const;

    /**
     * @brief 推送逻辑 S16 样本。
     *
     * 仅当完整帧组装完成时才调用 sink；若 sink 失败，该帧已从组装器
     * 移除，调用方必须在自己有界队列指标中计入丢弃。
     * @param samples 样本缓冲区。
     * @param sample_count 样本数量。
     * @param sink 完整帧回调。
     * @return 处理成功返回 Ok。
     */
    Status Push(const int16_t* samples, std::size_t sample_count, const Sink& sink);

    /** @brief 清空未完成的组装状态。 */
    void Reset();

   private:
    voice::AudioFormat frame_format_;
    uint16_t hardware_period_ms_ = 0;
    std::size_t frame_samples_ = 0;
    int16_t* pending_samples_ = nullptr;
    std::shared_ptr<voice::AudioPayloadPool> payload_pool_;
    std::size_t pending_size_ = 0;
    bool prepared_ = false;
};

}  // namespace voicelife::audio_esp
