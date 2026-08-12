#include "voicelife/runtime/platform_assembly.h"

#include "platform_assemblies.h"
#include "support/test_support.h"

using voicelife::test::Check;

int main() {
    using voicelife::runtime::PlatformAssembly;
    using voicelife::runtime::SparkBotAssembly;
    using voicelife::runtime::VoiceLifePcbAssembly;

    // VoiceLife PCB：通过 PlatformAssembly 接口暴露可用的点阵文本显示，
    // 无图片/动画能力；Runtime 只依赖接口，不出现板型分支。
    VoiceLifePcbAssembly pcb_assembly;
    PlatformAssembly& pcb_as_interface = pcb_assembly;
    const auto& pcb_caps = pcb_as_interface.presentation().capabilities();
    Check(pcb_caps.available && pcb_caps.text, "VoiceLife PCB Assembly 必须暴露可用的文本显示");
    Check(!pcb_caps.static_image && !pcb_caps.animation && !pcb_caps.preview_image,
          "SSD1306 点阵屏不得声明图片/动画能力");

    // SSD1306 Adapter：Render 走旧渲染路径（host 下无副作用），Submit 明确不支持。
    const auto pcb_render = pcb_as_interface.presentation().Render(voicelife::voice::DisplaySnapshot{});
    Check(pcb_render.ok(), "点阵 Adapter 的 Render 契约路径必须可执行");
    const auto pcb_submit = pcb_as_interface.presentation().Submit(
        voicelife::voice::PresentationCommand{.asset_id = "idle", .generation = 0, .request_id = {}});
    Check(pcb_submit.code == voicelife::ErrorCode::kUnavailable,
          "SSD1306 点阵屏的资源命令必须返回 kUnavailable（能力不支持）");

    // SparkBot：完整显示链路（队列 -> 显示任务 -> Renderer），available=true。
    SparkBotAssembly sparkbot_assembly;
    PlatformAssembly& sparkbot_as_interface = sparkbot_assembly;
    const auto& sparkbot_caps = sparkbot_as_interface.presentation().capabilities();
    Check(sparkbot_caps.available && sparkbot_caps.text && sparkbot_caps.animation,
          "SparkBot Assembly 显示链路闭合后必须声明可用能力（实板未验证另行标注）");

    // Render 提交快照到有界队列：立即返回 Ok（异步渲染由显示任务执行）。
    voicelife::voice::DisplaySnapshot snapshot;
    snapshot.revision = 1;
    snapshot.status_text = "测试";
    Check(sparkbot_as_interface.presentation().Render(snapshot).ok(), "SparkBot Render 必须接受快照并入队");

    // Start() 生命周期：VoiceLife PCB 默认空实现成功；SparkBot 的
    // ST7789/LVGL 初始化与显示任务仅 ESP 构建启用，host 下返回
    // kUnavailable（不触碰硬件，不伪装成功）。
    Check(pcb_as_interface.Start().ok(), "VoiceLife PCB Assembly Start 必须成功（默认空实现）");
    const auto sparkbot_start = sparkbot_as_interface.Start();
    Check(sparkbot_start.code == voicelife::ErrorCode::kUnavailable,
          "SparkBot Assembly Start 在 host 构建必须返回 kUnavailable（不触碰硬件）");

    return 0;
}
