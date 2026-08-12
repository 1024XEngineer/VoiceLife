#include "voicelife/display_esp/sparkbot_presentation_adapter.h"

#include <algorithm>
#include <array>
#include <string>
#include <string_view>

namespace voicelife::display_esp {

namespace {

/** @brief SparkBot 官方牛头表情的受控标识列表（与 manifest.json 一致）。 */
constexpr std::array<std::string_view, 10> kSparkBotAssetIds = {
    "boot", "connecting", "error", "happy", "idle", "listening", "provisioning", "sleepy", "speaking", "thinking",
};

/** @brief SparkBot 彩屏能力声明：Renderer 未移植，全部能力为 false。 */
constexpr voicelife::voice::DisplayCapabilities kSparkBotCapabilities{
    .available = false,
    .text = false,
    .static_image = false,
    .animation = false,
    .preview_image = false,
    // 240x240 ST7789 RGB565 单帧缓冲的硬件上限，非当前可用值；官方
    // Renderer 移植并真机验证前，Runtime 不得据此分配资源。
    .max_frame_bytes = 240U * 240U * 2U,
    // 官方 LVGL 刷新节奏尚未移植核实；Renderer 移植时按上游 lvgl_display
    // 实现填写并补充真机证据，之前保持 0（未确认）。
    .refresh_budget_hz = 0U,
};

bool IsPathLike(std::string_view asset_id) {
    return asset_id.empty() || asset_id.find('/') != std::string_view::npos ||
           asset_id.find('\\') != std::string_view::npos || asset_id.find("..") != std::string_view::npos;
}

}  // namespace

voicelife::Result<SparkBotAssetId> ParseSparkBotAssetId(std::string_view asset_id) {
    if (IsPathLike(asset_id)) {
        return voicelife::Result<SparkBotAssetId>::Failure(voicelife::ErrorCode::kInvalidArgument,
                                                           "资源标识必须是非空、无路径分隔符的受控名称");
    }
    const auto it = std::find(kSparkBotAssetIds.begin(), kSparkBotAssetIds.end(), asset_id);
    if (it == kSparkBotAssetIds.end()) {
        return voicelife::Result<SparkBotAssetId>::Failure(voicelife::ErrorCode::kNotFound,
                                                           "资源不在官方 SparkBot 资源清单中");
    }
    const auto index = static_cast<std::size_t>(it - kSparkBotAssetIds.begin());
    return voicelife::Result<SparkBotAssetId>::Success(static_cast<SparkBotAssetId>(index));
}

const voicelife::voice::DisplayCapabilities& SparkBotPresentationAdapter::capabilities() const {
    return kSparkBotCapabilities;
}

voicelife::Status SparkBotPresentationAdapter::Render(const voicelife::voice::DisplaySnapshot& /*snapshot*/) {
    return voicelife::Status::Error(voicelife::ErrorCode::kUnavailable,
                                    "SparkBot LVGL Renderer 尚未移植；请以 xiaozhi-esp32 官方 esp-sparkbot "
                                    "显示实现（commit 37d1aee）为唯一来源完成移植后再启用");
}

voicelife::Status SparkBotPresentationAdapter::Submit(voicelife::voice::PresentationCommand command) {
    const auto parsed = ParseSparkBotAssetId(command.asset_id);
    if (!parsed.ok()) {
        return parsed.status;
    }
    // 受控资源已确认，但骨架不解析、不加载任何资源；asset_id 不会被当作
    // 路径或 URL 使用。
    return voicelife::Status::Error(voicelife::ErrorCode::kUnavailable,
                                    "SparkBot 显示资源加载尚未实现；官方 Renderer 移植完成前不接受任何资源命令");
}

}  // namespace voicelife::display_esp
