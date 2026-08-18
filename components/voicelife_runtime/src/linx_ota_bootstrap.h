#pragma once

#include <string>
#include <string_view>

#include "voicelife/contracts/status.h"
#include "voicelife/linx/linx_types.h"
#include "wifi_provisioning.h"

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
 * @brief 当前 ESP STA 是否已关联到 AP。
 *
 * 仅由运行时启动流程读取并转换为 DisplaySnapshot 的网络语义；显示 Adapter
 * 不直接依赖 ESP Wi-Fi API。主机构建恒为 false。
 */
bool LinxWifiStaConnected();

/**
 * @brief 在已连接的 ESP STA 上拉取 Linx OTA 配置并受控写入 NVS。
 * @return 可用于创建 WSS Transport 的配置，或网络、激活、协议和安全存储错误。
 */
Result<linx::LinxConnectionConfig> BootstrapLinxOtaConfig(
    std::string_view board_identity, const WifiProvisioningStatusSink& provisioning_status_sink = {});

/** Persist a physical re-provisioning request for the next boot without altering Wi-Fi credentials. */
Status RequestLinxWifiProvisioning();

}  // namespace voicelife::runtime
