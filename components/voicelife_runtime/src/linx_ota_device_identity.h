#pragma once

#ifdef ESP_PLATFORM

#include <cstddef>
#include <cstdint>
#include <string>

#include "voicelife/contracts/status.h"

namespace voicelife::runtime {

/** @brief 读取或在 Linx 安全分区中创建本安装的 Client ID。 */
Result<std::string> LoadOrCreateLinxClientId();
/** @brief 以 Linx OTA 所需的小写冒号分隔格式读取 STA MAC。 */
std::string ReadLinxOtaDeviceId();
/** @brief 将二进制摘要编码为小写十六进制。 */
std::string EncodeLinxOtaHexDigest(const uint8_t* digest, size_t size);

}  // namespace voicelife::runtime

#endif
