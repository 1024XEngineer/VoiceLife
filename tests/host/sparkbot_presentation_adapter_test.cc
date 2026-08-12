#include "voicelife/display_sparkbot/sparkbot_presentation_adapter.h"

#include "support/test_support.h"
#include "voicelife/voice/voice_ports.h"

using voicelife::ErrorCode;
using voicelife::test::Check;
using voicelife::voice::DisplaySnapshot;
using voicelife::voice::PresentationCommand;

int main() {
    using voicelife::display_sparkbot::SparkBotLcdConfig;
    using voicelife::display_sparkbot::SparkBotPresentationAdapter;

    SparkBotLcdConfig config;  // 官方默认参数（240x240 SPI mode 2）
    SparkBotPresentationAdapter adapter(config);

    // 显示链路已闭合：available=true，文本/静态图/动画能力声明。
    const auto& caps = adapter.capabilities();
    Check(caps.available, "显示链路闭合后 available 必须为 true");
    Check(caps.text && caps.static_image && caps.animation && !caps.preview_image,
          "能力声明必须包含文本/静态图/动画，且不含预览图");
    Check(caps.max_frame_bytes == 240U * 240U * 2U, "帧缓冲硬件上限必须为 240x240 RGB565");

    // Render 提交快照到有界队列：立即返回 Ok（异步渲染由显示任务执行）。
    DisplaySnapshot snapshot;
    snapshot.revision = 1;
    snapshot.mood = voicelife::voice::VoiceMood::kSpeaking;
    snapshot.status_text = "播报中";
    const auto render_status = adapter.Render(snapshot);
    Check(render_status.ok(), "Render 必须接受快照并入队");

    // Submit 契约：非法格式 kInvalidArgument、未知资源 kNotFound、
    // 受控资源 kUnavailable（独立资源命令未实现）。
    Check(adapter.Submit(PresentationCommand{.asset_id = "", .request_id = {}}).code == ErrorCode::kInvalidArgument,
          "空 asset_id 必须返回 kInvalidArgument");
    Check(adapter.Submit(PresentationCommand{.asset_id = "../evil.gif", .request_id = {}}).code ==
              ErrorCode::kInvalidArgument,
          "路径特征 asset_id 必须返回 kInvalidArgument");
    Check(adapter.Submit(PresentationCommand{.asset_id = "not_in_manifest", .request_id = {}}).code ==
              ErrorCode::kNotFound,
          "未知资源必须返回 kNotFound");
    Check(adapter.Submit(PresentationCommand{.asset_id = "idle", .request_id = {}}).code == ErrorCode::kUnavailable,
          "受控资源命令未实现必须返回 kUnavailable");

    // host 构建不启动真实显示任务。
    Check(adapter.Start().code == ErrorCode::kUnavailable,
          "host 构建 Start 必须返回 kUnavailable（不创建真实显示任务）");
    Check(adapter.Stop().ok(), "host 构建 Stop 必须成功（无任务可停）");

    // 背光仲裁回调：待机快照关闭背光，非待机恢复。
    bool backlight_requests[2] = {false, false};
    int backlight_calls = 0;
    SparkBotPresentationAdapter backlight_adapter(config, [&](bool on) {
        if (backlight_calls < 2) {
            backlight_requests[backlight_calls] = on;
        }
        ++backlight_calls;
    });
    DisplaySnapshot standby;
    standby.revision = 2;
    standby.phase = voicelife::voice::VoiceInteractionState::kStandby;
    Check(backlight_adapter.Render(standby).ok(), "待机快照必须可提交");
    Check(backlight_calls == 1 && !backlight_requests[0], "待机必须请求关闭背光（经板级仲裁）");
    DisplaySnapshot listening;
    listening.revision = 3;
    listening.phase = voicelife::voice::VoiceInteractionState::kListening;
    Check(backlight_adapter.Render(listening).ok(), "聆听快照必须可提交");
    Check(backlight_calls == 2 && backlight_requests[1], "非待机必须请求开启背光");
    Check(backlight_adapter.Render(listening).ok() && backlight_calls == 2, "相同背光状态不得重复请求");

    return 0;
}
