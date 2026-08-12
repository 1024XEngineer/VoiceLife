#include "runtime_linx_bootstrap.h"

#ifdef ESP_PLATFORM

#include <utility>

#include "esp_log.h"
#include "linx_mcp_bridge.h"
#include "linx_ota_bootstrap.h"
#include "schedule_mcp_tools.h"
#include "voicelife/linx/linx_speech_provider.h"

namespace voicelife::runtime {
namespace {

constexpr char kTag[] = "VoiceLifeRuntime";
constexpr char kProviderId[] = "xrobot-websocket";

}  // namespace

Status RuntimeLinxBootstrap::Initialize() {
    const Status tool_status = RegisterScheduleMcpTools(mcp_server_, schedule_service_);
    if (tool_status.ok()) {
        ESP_LOGI(kTag, "MCP_TOOLS_READY count=2 names=schedule.create,schedule.query");
    }
    auto& registry = voice::SpeechProviderRegistry::Instance();
    registry.Register(kProviderId, linx::LinxSpeechProviderAdapter::DefaultCapabilities(), [this]() {
        return std::make_unique<linx::LinxSpeechProviderAdapter>(
            *transport_, codec_, connection_config_, linx::LinxSpeechProviderAdapter::DefaultCapabilities(),
            [this](std::string_view payload, std::string_view session_id) {
                return HandleLinxMcpPayload(payload, mcp_server_, session_id);
            });
    });
    return tool_status;
}

Status RuntimeLinxBootstrap::InitializeSecretStore() { return InitializeLinxSecretStore(); }

Status RuntimeLinxBootstrap::LoadOtaConnectionConfig() {
    auto connection = BootstrapLinxOtaConfig();
    if (!connection.ok() || !connection.value.has_value()) {
        return connection.status;
    }
    connection_config_ = std::move(*connection.value);
    return Status::Ok();
}

Result<std::unique_ptr<voice::SpeechProviderAdapter>> RuntimeLinxBootstrap::CreateProvider() const {
    return voice::SpeechProviderRegistry::Instance().Create(kProviderId, {});
}

}  // namespace voicelife::runtime

#endif
