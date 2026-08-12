#pragma once

#include <memory>
#include <string>

#include "voicelife/im/im_transport.h"

namespace voicelife::im {

/**
 * @brief 创建使用 ESP-IDF 系统 CA bundle 和主机名校验的 HTTPS Transport。
 * @param gateway_origin 已校验的 Gateway HTTPS origin。
 * @return 由调用方持有的 Transport；仅 ESP 固件提供实现。
 */
std::unique_ptr<ImTransport> CreateEspHttpTransport(std::string gateway_origin);

}  // namespace voicelife::im
