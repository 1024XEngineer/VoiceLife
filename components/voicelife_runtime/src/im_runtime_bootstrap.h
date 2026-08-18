#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "voicelife/contracts/status.h"
#include "voicelife/im/im_config_store.h"
#include "voicelife/im/im_pairing_client.h"
#include "voicelife/im/im_pairing_controller.h"
#include "voicelife/im/im_runtime.h"

namespace voicelife::runtime {

class EspPairingClock final : public im::ImPairingClock {
   public:
    uint64_t MonotonicMillis() const override;
    uint64_t UnixMillis() const override;
};

/// 从已初始化的 HMAC 加密 NVS 分区读取 IM 配置与凭据。
class NvsImSecretStore final : public im::ImSecretStorePort {
   public:
    /** @brief 读取 `im` namespace 中的字符串。 @param key 非敏感字段名。 @return 字段值或类型化失败。 */
    Result<std::string> Read(std::string_view key) override;
};

/// ESP32 Wi-Fi、DNS 与可信系统时间的 IM 就绪检查。
class EspImRuntimeReadiness final : public im::ImRuntimeReadinessPort {
   public:
    /** @brief Wi-Fi 已关联且 DHCP 提供 DNS 时为 true。 @return 网络前置条件。 */
    bool NetworkReady() const override;
    /** @brief 实时时钟达到可信最小 epoch 时为 true。 @return TLS 时间前置条件。 */
    bool SystemTimeReady() const override;
};

/**
 * @brief 启动一次受控物理串口 provisioning 窗口。
 *
 * 启用 IM 的 profile 始终开放该物理窗口；VLI1 仅允许首次配置，VLI2
 * 显式允许覆盖已有配置。成功后写入加密 NVS 并重启。
 * 重复调用不会创建重复任务。
 *
 * @return 任务已存在或创建成功时为 true。
 */
bool StartImProvisioningTask();

/**
 * @brief 在 Gateway 探针成功后发布物理 USB 可用的配对端口。
 * @param client Runtime 持有且生命周期覆盖设备运行期的客户端。
 * @param device_id 已认证的非敏感设备标识。
 * @param user_id 已配置的非敏感用户引用。
 */
void RegisterImPairingAcceptance(im::ImPairingPort* client, std::string device_id, std::optional<std::string> user_id);

/**
 * @brief 幂等地启动并等待一次 SNTP 时间同步。
 *
 * 时间已可信时立即返回；否则初始化 DHCP/池化 SNTP 并有界等待同步事件。
 * 失败只降级 IM，不阻塞本地语音。可重复调用，不会产生重复任务。
 *
 * @return 时间同步成功或已可信时为 Ok。
 */
Status SynchronizeSystemTime();

}  // namespace voicelife::runtime
