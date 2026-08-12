#include "voicelife/display_esp/sparkbot_presentation_adapter.h"

#include "support/test_support.h"
#include "voicelife/voice/voice_ports.h"

using voicelife::ErrorCode;
using voicelife::test::Check;
using voicelife::voice::DisplaySnapshot;
using voicelife::voice::PresentationCommand;

int main() {
    using voicelife::display_esp::ParseSparkBotAssetId;
    using voicelife::display_esp::SparkBotAssetId;
    using voicelife::display_esp::SparkBotPresentationAdapter;

    SparkBotPresentationAdapter adapter;

    // 方案 A：Renderer 未移植时 available=false 且所有可运行能力均为 false，
    // 不伪装成已经支持文本/静态图/动画。
    const auto& caps = adapter.capabilities();
    Check(!caps.available, "官方 Renderer 未移植前 available 必须为 false");
    Check(!caps.text && !caps.static_image && !caps.animation && !caps.preview_image,
          "available=false 时所有可运行能力必须为 false");
    Check(caps.max_frame_bytes == 240U * 240U * 2U, "max_frame_bytes 必须保留 240x240 RGB565 硬件上限参考");
    Check(caps.refresh_budget_hz == 0U, "官方刷新节奏未移植核实前必须保持 0（未确认）");

    const auto render_status = adapter.Render(DisplaySnapshot{});
    Check(render_status.code == ErrorCode::kUnavailable, "骨架 Render 必须返回 kUnavailable，不能静默假装渲染成功");

    // Submit 契约：受控资源 -> kUnavailable；非法格式 -> kInvalidArgument；
    // 未知资源 -> kNotFound。
    const auto controlled = adapter.Submit(PresentationCommand{.asset_id = "boot", .generation = 0, .request_id = {}});
    Check(controlled.code == ErrorCode::kUnavailable, "受控资源在 Renderer 未移植时必须返回 kUnavailable");

    const auto empty = adapter.Submit(PresentationCommand{.asset_id = "", .generation = 0, .request_id = {}});
    Check(empty.code == ErrorCode::kInvalidArgument, "空 asset_id 必须返回 kInvalidArgument");
    const auto path_like =
        adapter.Submit(PresentationCommand{.asset_id = "../arbitrary/path.gif", .generation = 0, .request_id = {}});
    Check(path_like.code == ErrorCode::kInvalidArgument, "含路径特征的 asset_id 必须返回 kInvalidArgument");
    const auto url_like = adapter.Submit(
        PresentationCommand{.asset_id = "https://example.com/boot.gif", .generation = 0, .request_id = {}});
    Check(url_like.code == ErrorCode::kInvalidArgument, "URL 形式的输入必须返回 kInvalidArgument");
    const auto abs_path =
        adapter.Submit(PresentationCommand{.asset_id = "/etc/passwd", .generation = 0, .request_id = {}});
    Check(abs_path.code == ErrorCode::kInvalidArgument, "绝对路径输入必须返回 kInvalidArgument");
    const auto backslash = adapter.Submit(PresentationCommand{.asset_id = "a\\b", .generation = 0, .request_id = {}});
    Check(backslash.code == ErrorCode::kInvalidArgument, "反斜杠路径输入必须返回 kInvalidArgument");
    const auto unknown =
        adapter.Submit(PresentationCommand{.asset_id = "not_a_real_id", .generation = 0, .request_id = {}});
    Check(unknown.code == ErrorCode::kNotFound, "未知资源必须返回 kNotFound");

    // 受控标识解析：全部官方 asset_id 必须可解析，且与枚举一一对应。
    const std::string_view kAllIds[] = {
        "boot", "connecting", "error", "happy", "idle", "listening", "provisioning", "sleepy", "speaking", "thinking",
    };
    for (std::size_t i = 0; i < sizeof(kAllIds) / sizeof(kAllIds[0]); ++i) {
        const auto parsed = ParseSparkBotAssetId(kAllIds[i]);
        Check(parsed.ok() && parsed.value.has_value() && *parsed.value == static_cast<SparkBotAssetId>(i),
              "官方 asset_id 必须可解析为受控枚举");
    }
    Check(!ParseSparkBotAssetId("not_in_list").ok() &&
              ParseSparkBotAssetId("not_in_list").status.code == ErrorCode::kNotFound,
          "清单外的合法格式标识必须返回 kNotFound");

    return 0;
}
