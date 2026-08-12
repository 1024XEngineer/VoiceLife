#pragma once

#ifdef ESP_PLATFORM

#include "voicelife/linx_esp/esp_websocket_transport.h"

namespace voicelife::runtime {

/** @brief 从加密 NVS 解析 Linx Transport 所需的密钥引用。 */
class NvsSecretResolver final : public linx_esp::SecretResolverPort {
   public:
    /** @brief 解析 nvs://namespace/key 格式的引用。 */
    Result<std::string> Resolve(std::string_view reference) override;
};

}  // namespace voicelife::runtime

#endif
