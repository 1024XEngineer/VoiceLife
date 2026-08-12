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

    // 显示链路能力声明：硬件能力为 true，但 available 只在显示启动成功后置真；
    // host 下未启动，available 必须为 false（不产生“运行正常但屏幕不可用”假成功）。
    const auto& caps = adapter.capabilities();
    Check(!caps.available, "host 下未启动显示，available 必须为 false");
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
    // 生产运行期待机保持背光（idle GIF 可见）；省电需显式 DisplayPowerMode。
    DisplaySnapshot standby;
    standby.revision = 2;
    standby.phase = voicelife::voice::VoiceInteractionState::kStandby;
    Check(backlight_adapter.Render(standby).ok(), "待机快照必须可提交");
    Check(backlight_calls == 1 && backlight_requests[0], "首次渲染（含待机）必须请求开启背光，待机不关闭");
    DisplaySnapshot listening;
    listening.revision = 3;
    listening.phase = voicelife::voice::VoiceInteractionState::kListening;
    Check(backlight_adapter.Render(listening).ok(), "聆听快照必须可提交");
    Check(backlight_calls == 1 && backlight_requests[0], "首次渲染必须请求开启背光");
    Check(backlight_adapter.Render(listening).ok() && backlight_calls == 1, "背光保持开启不得重复请求");

    // generation -> revision 消费丢弃：四种顺序（P1 回归保护）。
    using voicelife::display_sparkbot::ShouldDropDisplaySnapshot;
    Check(ShouldDropDisplaySnapshot(1, 99, 2, 0), "旧 generation 必须整体拒绝");
    Check(ShouldDropDisplaySnapshot(2, 3, 2, 3), "同 generation 旧 revision 必须拒绝");
    Check(!ShouldDropDisplaySnapshot(2, 4, 2, 3), "同 generation 新 revision 必须接受");
    Check(!ShouldDropDisplaySnapshot(3, 0, 2, 99), "新 generation 首帧（revision 较小）必须接受");

    return 0;
}
