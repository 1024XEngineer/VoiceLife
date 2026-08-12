#pragma once

#include <string>
#include <string_view>

#include "voicelife/contracts/status.h"
#include "voicelife/im/im_credentials.h"
#include "voicelife/im/im_runtime.h"

namespace voicelife::im {

/// IM 配置在安全存储中的稳定字段名。
inline constexpr std::string_view kImGatewayOriginKey = "gateway_origin";
inline constexpr std::string_view kImDeviceIdKey = "device_id";
inline constexpr std::string_view kImDeviceTokenKey = "device_token";
inline constexpr std::string_view kImUserIdKey = "user_id";

/// 从平台安全存储读取单个 IM 配置字段的最小端口。
class ImSecretStorePort {
   public:
    /** @brief 允许通过接口指针释放安全存储实现。 */
    virtual ~ImSecretStorePort() = default;
    /**
     * @brief 读取一个 UTF-8 字符串字段。
     * @param key 稳定字段名，不包含 Secret 值。
     * @return 字段值；不存在时返回 kNotFound，读取失败返回 kUnavailable。
     */
    virtual Result<std::string> Read(std::string_view key) = 0;
};

/**
 * @brief 从安全存储原子加载 IM 配置，并同时提供设备凭据端口。
 *
 * 必填字段全部读取成功前不会发布 deviceId 或 token；禁用时完全不访问存储。
 */
class StoredImConfigProvider final : public ImConfigProvider, public ImCredentialProvider {
   public:
    /**
     * @brief 创建存储型配置 Provider。
     * @param store 平台安全存储。
     * @param enabled Profile 是否启用 IM Adapter。
     */
    StoredImConfigProvider(ImSecretStorePort& store, bool enabled) : store_(store), enabled_(enabled) {}
    /** @brief 清除内存中的设备 Token。 */
    ~StoredImConfigProvider() override;

    /** @brief 从安全存储原子加载 IM 配置。 @return 完整配置，或缺失/读取失败状态。 */
    Result<ImRuntimeConfig> Load() override;
    /** @brief 返回已加载的设备 Token。 @return 未完整加载时为空。 */
    std::string DeviceToken() const override { return device_token_; }
    /** @brief 返回已加载的设备 ID。 @return 未完整加载时为空。 */
    std::string DeviceId() const override { return device_id_; }

   private:
    void ClearCredentials();

    ImSecretStorePort& store_;
    bool enabled_ = false;
    std::string device_id_;
    std::string device_token_;
};

}  // namespace voicelife::im
