#pragma once

#include <string>

#include "esp_http_client.h"
#include "voicelife/im/im_transport.h"

namespace voicelife::im {

/// 基于 ESP-IDF esp_http_client 的 HTTPS 传输实现。仅固件编译。
class EspHttpTransport : public ImTransport {
   public:
    /** @brief 创建传输实例。 @param base_url 网关基地址，例如 "https://im.example.com"。 */
    explicit EspHttpTransport(std::string base_url);
    /**
     * @brief 通过 esp_http_client 提交 HTTPS POST。
     * @param request 目标路径、头与请求体。
     * @return 传输结果，网络层失败映射为 kNetworkFailure。
     */
    ImHttpResponse Post(const ImHttpRequest& request) override;
    /** @brief 通过 esp_http_client 执行 HTTPS GET。 */
    ImHttpResponse Get(const ImHttpRequest& request) override;

   private:
    ImHttpResponse Perform(const ImHttpRequest& request, esp_http_client_method_t method);
    std::string base_url_;
};

}  // namespace voicelife::im
