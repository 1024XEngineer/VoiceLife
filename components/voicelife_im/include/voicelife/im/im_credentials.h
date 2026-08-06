#pragma once

#include <string>

namespace voicelife::im {

/// 设备凭据提供端口。凭据通过部署环境注入，不进入 Profile 或日志。
class ImCredentialProvider {
   public:
    /** @brief 允许通过接口指针释放凭据提供者。 */
    virtual ~ImCredentialProvider() = default;
    /**
     * @brief 返回设备访问令牌。
     * @return 访问令牌；未配置时返回空串。
     */
    virtual std::string DeviceToken() const = 0;
    /**
     * @brief 返回设备标识。
     * @return 设备标识 deviceId。
     */
    virtual std::string DeviceId() const = 0;
};

}  // namespace voicelife::im
