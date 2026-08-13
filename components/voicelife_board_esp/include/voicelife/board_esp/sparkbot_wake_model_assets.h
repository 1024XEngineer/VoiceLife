#pragma once

#include <cstddef>

#include "voicelife/contracts/status.h"

namespace voicelife::board_esp {

/**
 * SparkBot 受控 WakeNet 模型资源。
 *
 * 模型仅从构建期固定的 assets 分区加载；它不接受 URL、路径或调用方字节流。
 * mmap 生命周期由板级 Assembly 持有，Runtime、VoiceSession 和领域层不接触模型
 * 文件或 ESP-SR 类型。
 */
class SparkBotWakeModelAssets {
   public:
    SparkBotWakeModelAssets() = default;
    ~SparkBotWakeModelAssets();

    SparkBotWakeModelAssets(const SparkBotWakeModelAssets&) = delete;
    SparkBotWakeModelAssets& operator=(const SparkBotWakeModelAssets&) = delete;

    /** mmap assets 并验证固定 srmodels.bin 记录。 */
    [[nodiscard]] Status Initialize();
    /** 已验证的 srmodels.bin 根地址；未初始化时为 nullptr。 */
    [[nodiscard]] const void* model_root() const { return model_root_; }
    /** 已验证模型镜像大小。 */
    [[nodiscard]] std::size_t model_size() const { return model_size_; }

   private:
    [[maybe_unused]] const void* mmap_root_ = nullptr;
    [[maybe_unused]] void* mmap_handle_ = nullptr;
    const void* model_root_ = nullptr;
    std::size_t model_size_ = 0;
};

}  // namespace voicelife::board_esp
