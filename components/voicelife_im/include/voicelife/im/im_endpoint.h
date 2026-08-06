#pragma once

#include <string>

namespace voicelife::im {

/**
 * @brief 校验网关基地址是否可直接用于 HTTPS 上报。
 * @param base_url 网关基地址，例如 "https://im.example.com"。
 * @return 仅当以 https:// 开头且不含 query 与 fragment 时返回 true。
 */
bool IsHttpsGatewayUrl(const std::string& base_url);

}  // namespace voicelife::im
