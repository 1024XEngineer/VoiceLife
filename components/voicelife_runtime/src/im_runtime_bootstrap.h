#pragma once

#include <string>
#include <string_view>

#include "voicelife/contracts/status.h"
#include "voicelife/im/im_config_store.h"
#include "voicelife/im/im_runtime.h"

namespace voicelife::runtime {

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
 * 任务仅在配置缺失时由 Runtime 调用；成功后写入加密 NVS 并重启。
 * 重复调用不会创建重复任务。
 *
 * @return 任务已存在或创建成功时为 true。
 */
bool StartImProvisioningTask();

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
