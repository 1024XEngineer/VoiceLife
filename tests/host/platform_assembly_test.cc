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

    // SparkBot：完整显示链路（队列 -> 显示任务 -> Renderer）。available 只在
    // 显示启动成功后置真；host 下未启动必须为 false（不产生假成功）。
    SparkBotAssembly sparkbot_assembly;
    PlatformAssembly& sparkbot_as_interface = sparkbot_assembly;
    const auto& sparkbot_caps = sparkbot_as_interface.presentation().capabilities();
    Check(!sparkbot_caps.available && sparkbot_caps.text && sparkbot_caps.animation,
          "SparkBot 显示启动前 available 必须为 false，能力声明保留文本/动画");

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

    // 板级注入：音频 Profile 与按键 GPIO 由各 Assembly 构建期提供，
    // Runtime 不固定板型（SparkBot 不得重配 LCD/音频复用引脚）。
    Check(pcb_as_interface.audio_profile().id == "esp32s3-voicelife-pcb-pcm",
          "VoiceLife PCB 必须注入 PCM 音频 Profile");
    const auto pcb_buttons = pcb_as_interface.button_gpios();
    Check(pcb_buttons == std::vector<int>({0, 47, 40, 39}), "VoiceLife PCB 必须注入 boot/touch/volume 按键 GPIO");
    Check(sparkbot_as_interface.audio_profile().id == "esp32s3-esp-sparkbot",
          "SparkBot 必须注入 ES8311 双工音频 Profile");
    const auto sparkbot_buttons = sparkbot_as_interface.button_gpios();
    Check(sparkbot_buttons == std::vector<int>({0}), "SparkBot 只能注入 BOOT 按键，不得包含 LCD/音频复用引脚");
    Check(sparkbot_as_interface.SetAudioOutputEnabled(true).ok(), "SparkBot 音频功放请求必须经仲裁接口接受");

    return 0;
}
