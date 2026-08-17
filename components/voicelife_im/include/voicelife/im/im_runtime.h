#pragma once

#include <ctime>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "voicelife/contracts/status.h"
#include "voicelife/im/im_credentials.h"
#include "voicelife/im/im_pairing_client.h"
#include "voicelife/im/im_reporting_channel.h"
#include "voicelife/im/im_transport.h"

namespace voicelife::im {

/// 设备 IM Adapter 的非 Secret 运行时配置。
struct ImRuntimeConfig {
    /// false 时完全禁用 IM，不读取凭据也不创建网络资源。
    bool enabled = false;
    /// Gateway HTTPS origin；不得包含路径、query、fragment 或用户信息。
    std::string gateway_origin;
    /// 可选的本地用户引用，供后续配对会话使用。
    std::optional<std::string> user_id;
};

/// 提供设备 IM 运行时配置的端口。
class ImConfigProvider {
   public:
    /** @brief 允许通过接口指针释放配置提供者。 */
    virtual ~ImConfigProvider() = default;
    /**
     * @brief 读取当前 IM 配置。
     * @return 配置值；缺失或安全存储不可用时返回类型化失败。
     */
    virtual Result<ImRuntimeConfig> Load() = 0;
};

/// 提供创建 TLS Transport 前必须满足的设备就绪条件。
class ImRuntimeReadinessPort {
   public:
    /** @brief 允许通过接口指针释放就绪检查实现。 */
    virtual ~ImRuntimeReadinessPort() = default;
    /** @brief 判断设备网络是否已获得可用连接。 @return 已联网时为 true。 */
    virtual bool NetworkReady() const = 0;
    /** @brief 判断系统时间是否足以执行证书有效期校验。 @return 时间可信时为 true。 */
    virtual bool SystemTimeReady() const = 0;
};

/// IM 基础运行时的稳定状态；不表达任何 Schedule 或 TimingTask 业务状态。
enum class ImRuntimeState {
    /// Profile 或配置明确禁用 IM。
    kDisabled,
    /// 配置或设备凭据缺失、非法。
    kUnconfigured,
    /// 本地前置条件满足，正在验证 Gateway 与设备凭据。
    kProbing,
    /// Gateway 认证探针成功，基础组件已创建。
    kReady,
    /// 网络、时间或 Transport 初始化暂不可用。
    kDegraded,
};

/// 根据受控 Gateway origin 创建设备 HTTPS Transport 的工厂。
using ImTransportFactory = std::function<std::unique_ptr<ImTransport>(const std::string& gateway_origin)>;

/// 可信任的参考时刻（2024-01-01T00:00:00Z）：TLS 证书有效期校验与
/// IM 动作窗口都需要系统时间已真实同步，而非 1970 默认 epoch。
inline constexpr time_t kTrustedEpochMinimum = 1704067200;

/**
 * @brief 判定系统时间是否足以执行 TLS 与契约时间校验。
 * @param now 当前系统时间（epoch 秒）。
 * @return 时间达到可信任参考时刻时为 true。
 */
bool IsTrustedSystemTime(time_t now);

/**
 * @brief 持有设备 IM 基础组件并提供 fail-closed、幂等启动语义。
 *
 * 本类型只装配配置、凭据、HTTPS Transport 与上报通道；不会主动发送
 * ScheduleReceipt、Notification，不会启动 SSE 或后台轮询任务。
 */
class ImRuntime {
   public:
    /**
     * @brief 创建尚未启动的 IM Runtime。
     * @param config 配置来源。
     * @param credentials 设备身份与 Bearer Token 来源。
     * @param readiness 网络与系统时间就绪检查。
     * @param transport_factory ESP 或测试 Transport 工厂。
     */
    ImRuntime(ImConfigProvider& config, ImCredentialProvider& credentials, ImRuntimeReadinessPort& readiness,
              ImTransportFactory transport_factory);

    /**
     * @brief 幂等装配基础组件。
     * @return ready/disabled 时成功；配置或暂时性前置条件失败时返回类型化错误。
     */
    Status Start();

    /**
     * @brief 通过无副作用的认证 GET 请求验证 Gateway 可达性和设备凭据。
     * @return 真实 Transport 结果；仅 2xx 或已认证后的 404 令 Runtime ready。
     */
    ImHttpResponse ProbeGateway();

    /** @brief 返回当前稳定状态。 @return 最近一次启动决策产生的状态。 */
    [[nodiscard]] ImRuntimeState state() const { return state_; }

    /**
     * @brief 返回 ready 状态下由 Runtime 持有的上报通道。
     * @return 未 ready 时为 nullptr；调用方不得取得所有权。
     */
    [[nodiscard]] ImReportingChannel* reporting_channel() const { return reporting_.get(); }

    /**
     * @brief 返回 ready 状态下由 Runtime 持有的配对客户端。
     * @return 未 ready 时为 nullptr；普通启动不会调用该客户端创建会话。
     */
    [[nodiscard]] ImPairingClient* pairing_client() const { return pairing_.get(); }

    /** @brief 返回已加载的可选用户引用。 @return 不包含 Secret 的 userId。 */
    [[nodiscard]] const std::optional<std::string>& user_id() const { return user_id_; }

   private:
    ImConfigProvider& config_;
    ImCredentialProvider& credentials_;
    ImRuntimeReadinessPort& readiness_;
    ImTransportFactory transport_factory_;
    ImRuntimeState state_ = ImRuntimeState::kUnconfigured;
    Status start_status_ = Status::Error(ErrorCode::kUnavailable, "IM Runtime 尚未启动");
    std::unique_ptr<ImTransport> transport_;
    std::unique_ptr<ImReportingChannel> reporting_;
    std::unique_ptr<ImPairingClient> pairing_;
    std::optional<std::string> user_id_;
};

}  // namespace voicelife::im
