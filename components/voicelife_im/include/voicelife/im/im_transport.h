#pragma once

#include <string>
#include <vector>

namespace voicelife::im {

/// 一个 HTTPS 请求头。
struct ImHttpHeader {
    /// 头名称，例如 "Content-Type"。
    std::string name;
    /// 头值，例如 "application/json"。
    std::string value;
};

/// 提交给网关的一次 HTTPS 请求。
struct ImHttpRequest {
    /// 网关路径，例如 "/v1/im/notifications"。
    std::string path;
    /// HTTP 方法，例如 "GET" 或 "POST"。
    std::string method;
    /// 请求头列表。
    std::vector<ImHttpHeader> headers;
    /// 请求体，序列化后的 JSON 文本。
    std::string body;
};

/// HTTPS 提交结果的分类，用于驱动重试与降级决策。
enum class ImTransportStatus {
    /// 服务端受理成功（2xx）。
    kSuccess,
    /// 服务端以 401/403 拒绝，凭据或签名问题。
    kCredentialRejected,
    /// 服务端返回其他非 2xx 状态。
    kHttpError,
    /// 连接失败、超时等网络层失败。
    kNetworkFailure,
    /// 客户端配置错误（例如非 https 网关地址），不可重试。
    kInvalidConfig,
};

/// 一次 HTTPS 提交的结果。
struct ImHttpResponse {
    /// 传输结果分类。
    ImTransportStatus status = ImTransportStatus::kNetworkFailure;
    /// 服务端返回的 HTTP 状态码，网络失败时为 0。
    int status_code = 0;
    /// 服务端响应体，网络失败或空响应时为空字符串。
    std::string body;
    /// 面向人的失败说明。
    std::string message;
};

/// 平台无关的 HTTPS 提交端口。ESP-IDF 实现基于 esp_http_client。
class ImTransport {
   public:
    /** @brief 允许通过接口指针释放传输实现。 */
    virtual ~ImTransport() = default;
    /**
     * @brief 提交一次 HTTPS POST 请求。
     * @param request 目标路径、头与请求体。
     * @return 传输结果，含分类与状态码。
     */
    virtual ImHttpResponse Post(const ImHttpRequest& request) = 0;
    /**
     * @brief 执行一次 HTTPS GET 请求。
     * @param request 目标路径与请求头；请求体应为空。
     * @return 传输结果，含分类与状态码。
     */
    virtual ImHttpResponse Get(const ImHttpRequest& request) = 0;
};

}  // namespace voicelife::im
