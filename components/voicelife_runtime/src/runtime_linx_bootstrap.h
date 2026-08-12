#pragma once

#ifdef ESP_PLATFORM

#include <memory>

#include "linx_secret_resolver.h"
#include "voicelife/linx/linx_types.h"
#include "voicelife/linx_esp/esp_websocket_transport.h"
#include "voicelife/mcp/mcp_server.h"
#include "voicelife/schedule/schedule_service.h"
#include "voicelife/voice/voice_ports.h"

namespace voicelife::runtime {

/** @brief VoiceLife PCB 当前 Linx 连接的私有启动协调器。 */
class RuntimeLinxBootstrap final {
   public:
    /** @brief 注册 MCP 工具和 Linx Provider 工厂。 */
    Status Initialize();
    /** @brief 初始化旧板加密 NVS 密钥存储。 */
    Status InitializeSecretStore();
    /** @brief 从 OTA 配置装载 Linx 连接参数。 */
    Status LoadOtaConnectionConfig();
    /** @brief 创建已注册的 Linx Provider。 */
    Result<std::unique_ptr<voice::SpeechProviderAdapter>> CreateProvider() const;

   private:
    NvsSecretResolver secrets_;
    mcp::McpServer mcp_server_;
    schedule::ScheduleService schedule_service_;
    linx::LinxJsonCodec codec_;
    linx::LinxConnectionConfig connection_config_;
    std::unique_ptr<linx_esp::EspWebSocketTransport> transport_ =
        std::make_unique<linx_esp::EspWebSocketTransport>(secrets_);
};

}  // namespace voicelife::runtime

#endif
