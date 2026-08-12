#pragma once

#include <string>
#include <string_view>

#include "voicelife/contracts/status.h"
#include "voicelife/linx/linx_types.h"

namespace voicelife::runtime {

/**
 * @brief 初始化独立的 HMAC 加密 Linx 凭据分区，不触碰旧固件的默认 NVS。
 */
Status InitializeLinxSecretStore();

/**
 * @brief 返回承载 Wi-Fi 与 Linx 凭据的独立 NVS 分区标签。
 */
const char* LinxSecretPartitionLabel();

/**
 * @brief 在已连接的 ESP STA 上拉取 Linx OTA 配置并受控写入 NVS。
 * @return 可用于创建 WSS Transport 的配置，或网络、激活、协议和安全存储错误。
 */
Result<linx::LinxConnectionConfig> BootstrapLinxOtaConfig(std::string_view board_identity);

}  // namespace voicelife::runtime
