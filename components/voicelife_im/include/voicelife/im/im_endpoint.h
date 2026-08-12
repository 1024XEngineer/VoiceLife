#pragma once

#include <string>

namespace voicelife::im {

/**
 * @brief 校验网关基地址是否可直接用于 HTTPS 上报。
 * @param base_url 网关基地址，例如 "https://im.example.com"。
 * @return 仅当为不含 path、userinfo、query、fragment 的合法 HTTPS origin 时返回 true。
 */
bool IsHttpsGatewayUrl(const std::string& base_url);

}  // namespace voicelife::im
