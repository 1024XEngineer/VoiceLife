#pragma once

#include <optional>
#include <string>

#include "voicelife/contracts/im/pairing_session.h"
#include "voicelife/im/im_credentials.h"
#include "voicelife/im/im_transport.h"

namespace voicelife::im {

/// 创建配对会话时由本地交互层提供的选项。
struct PairingCreateOptions {
    std::optional<std::string> user_id;
    int expires_in_minutes = 10;
};

/// 配对 HTTP 边界的稳定结果分类。
enum class PairingClientStatus {
    kSuccess,
    kCredentialRejected,
    kNotFound,
    kRetryable,
    kRejected,
    kInvalidResponse,
};

/// 创建会话的结果。
struct PairingCreateResult {
    PairingClientStatus status = PairingClientStatus::kRejected;
    std::optional<contracts::im::CreatedPairingSession> value;
    std::string message;
};

/// 查询会话的结果。
struct PairingQueryResult {
    PairingClientStatus status = PairingClientStatus::kRejected;
    std::optional<contracts::im::PairingSessionStatus> value;
    std::string message;
};

/// 平台无关的配对端口，便于 Runtime 和主机测试替换实现。
class ImPairingPort {
   public:
    /** @brief 允许通过接口指针释放配对端口实现。 */
    virtual ~ImPairingPort() = default;
    /** @brief 创建一个短期微信配对会话。 @param options 用户引用与有效期。 @return 创建结果。 */
    virtual PairingCreateResult Create(const PairingCreateOptions& options) = 0;
    /** @brief 查询归属于当前设备的配对会话。 @param pairing_session_id 会话 ID。 @return 查询结果。 */
    virtual PairingQueryResult Query(const std::string& pairing_session_id) = 0;
};

/// 复用 ImTransport 的 Gateway 配对客户端。
class ImPairingClient final : public ImPairingPort {
   public:
    /** @brief 绑定现有 HTTPS Transport 与设备凭据。 @param transport HTTPS 端口。 @param credentials 设备身份。 */
    ImPairingClient(ImTransport& transport, ImCredentialProvider& credentials);
    /** @brief POST 创建固定为 wechat_official 的配对会话。 @param options 创建选项。 @return 创建结果。 */
    PairingCreateResult Create(const PairingCreateOptions& options) override;
    /** @brief GET 查询状态，并编码 session id。 @param pairing_session_id 会话 ID。 @return 查询结果。 */
    PairingQueryResult Query(const std::string& pairing_session_id) override;

   private:
    ImTransport& transport_;
    ImCredentialProvider& credentials_;
};

}  // namespace voicelife::im
