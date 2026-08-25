#include "voicelife/mcp/audio_mcp_tools.h"

#include <utility>

#include "voicelife/contracts/tool.h"
#include "voicelife/mcp/mcp_server.h"

namespace voicelife::mcp {

Status RegisterAudioMcpTools(McpServer& server, std::function<void(int)> set_volume) {
    if (!set_volume) return Status::Error(ErrorCode::kInvalidArgument, "音量设置回调不能为空");
    return server.add_tool(
        "self.audio_speaker.set_volume",
        "设置扬声器音量，范围为 0 到 100。若当前音量未知，先调用 self.get_device_status。",
        PropertyList({Property::WithIntegerRange("volume", 0, 100)}),
        [set_volume = std::move(set_volume)](const PropertyList& properties) {
            const auto volume = properties.value<int64_t>("volume");
            if (!volume.has_value()) {
                return ToolResult::Failure(Status::Error(ErrorCode::kInvalidArgument, "缺少 volume 参数"));
            }
            set_volume(static_cast<int>(*volume));
            return ToolResult::Success(ToolOutputValue::Boolean(true));
        });
}

}  // namespace voicelife::mcp
