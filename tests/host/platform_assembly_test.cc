#include "voicelife/runtime/platform_assembly.h"

#include <string>

#include "platform_assemblies.h"
#include "support/test_support.h"
#include "voicelife/display_esp/ssd1306_presentation_adapter.h"

using voicelife::test::Check;

namespace {

int g_display_initializations = 0;

voicelife::Status InitializeDisplaySuccessfully() {
    ++g_display_initializations;
    return voicelife::Status::Ok();
}

voicelife::Status InitializeDisplayFailure() {
    ++g_display_initializations;
    return voicelife::Status::Error(voicelife::ErrorCode::kUnavailable, "display unavailable");
}

}  // namespace

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

    // SSD1306 初始化是受控边界：必须实际调用面板初始化，并保留失败状态。
    using voicelife::display_esp::Ssd1306ContentFitsLine;
    using voicelife::display_esp::Ssd1306PresentationAdapter;
    Check(Ssd1306ContentFitsLine("12345678"), "8 位 ASCII 配网密码必须完整显示而不滚动");
    Check(Ssd1306ContentFitsLine("123456789012"), "12 位 ASCII 必须刚好填满内容栏");
    Check(!Ssd1306ContentFitsLine("1234567890123"), "超过内容栏宽度的 ASCII 必须滚动");
    Check(Ssd1306ContentFitsLine("一二三四五六"), "6 个中文字符应刚好填满内容栏");
    Check(!Ssd1306ContentFitsLine("一二三四五六七"), "7 个中文字符必须滚动，保留 6 字绑定码布局");
    Check(Ssd1306ContentFitsLine("\xc2\xa9"), "2 字节 UTF-8 字符必须按宽字符处理");
    Check(Ssd1306ContentFitsLine("\xe4\xb8\xad"), "3 字节 UTF-8 字符必须按宽字符处理");
    Check(Ssd1306ContentFitsLine("\xf0\x9f\x98\x80"), "4 字节 UTF-8 字符必须按宽字符处理");
    Check(Ssd1306ContentFitsLine(std::string(1, static_cast<char>(0xff))), "非法 UTF-8 首字节必须安全处理");
    Check(Ssd1306ContentFitsLine(std::string("\xe4\xb8")), "截断的 3 字节 UTF-8 应按剩余字节安全处理");
    Ssd1306PresentationAdapter initialized_display(&InitializeDisplaySuccessfully);
    Check(initialized_display.Start().ok() && g_display_initializations == 1, "SSD1306 Start 必须调用面板初始化");
    Ssd1306PresentationAdapter unavailable_display(&InitializeDisplayFailure);
    Check(unavailable_display.Start().code == voicelife::ErrorCode::kUnavailable && g_display_initializations == 2,
          "SSD1306 Start 必须传播面板初始化失败");

    // Start() 生命周期：VoiceLife PCB 初始化 SSD1306；SparkBot 的
    // ST7789/LVGL 初始化与显示任务仅 ESP 构建启用，host 下返回
    // kUnavailable（不触碰硬件，不伪装成功）。
    Check(pcb_as_interface.Start().ok(), "VoiceLife PCB Assembly Start 必须成功（默认空实现）");
    const auto sparkbot_start = sparkbot_as_interface.Start();
    Check(sparkbot_start.code == voicelife::ErrorCode::kUnavailable,
          "SparkBot Assembly Start 在 host 构建必须返回 kUnavailable（不触碰硬件）");

    // 板级输入：Assembly 持有 GPIO 与物理映射，Runtime 只能接收语义事件。
    bool pcb_input_started = false;
    Check(
        pcb_as_interface.StartBoardInput([&](voicelife::runtime::BoardInputAction) { pcb_input_started = true; }).ok(),
        "VoiceLife PCB 输入适配器必须可由 Assembly 启动");
    Check(!pcb_input_started, "host 构建不得伪造 PCB 物理按键事件");
    Check(pcb_as_interface.uses_local_wake_detector(), "VoiceLife PCB 必须保留本地唤醒模型待机能力");
    Check(pcb_as_interface.wake_gate().Open(voicelife::voice::AudioFormat{}).code == voicelife::ErrorCode::kUnavailable,
          "VoiceLife PCB 必须实际装配 WakeGate；host 下应到达受控硬件不可用路径而非空指针");
    bool sparkbot_input_started = false;
    Check(sparkbot_as_interface
              .StartBoardInput([&](voicelife::runtime::BoardInputAction) { sparkbot_input_started = true; })
              .ok(),
          "SparkBot 输入适配器必须可由 Assembly 启动");
    Check(!sparkbot_input_started, "host 构建不得伪造 SparkBot BOOT 事件");
    Check(!sparkbot_as_interface.uses_local_wake_detector(),
          "SparkBot 在真实 assets/WakeNet 未启动前不得错误声明本地唤醒就绪");
    Check(sparkbot_as_interface.SetAudioOutputEnabled(true).ok(), "SparkBot 音频功放请求必须经仲裁接口接受");

    // 显式调用基类默认实现，固定无硬件能力时的空操作契约。
    Check(pcb_assembly.PlatformAssembly::test_audio_injection() == nullptr, "基类默认测试注入端口应为空");
    Check(pcb_assembly.PlatformAssembly::uses_local_wake_detector(), "基类默认应启用本地唤醒检测");
    pcb_assembly.PlatformAssembly::InitializeBoardLeds();
    pcb_assembly.PlatformAssembly::SetOutputVolume(42);
    pcb_assembly.PlatformAssembly::LogAudioStats();
    Check(pcb_assembly.PlatformAssembly::StartBoardInput({}).ok(), "基类默认输入启动应成功");
    Check(pcb_assembly.PlatformAssembly::SetAudioOutputEnabled(false).ok(), "基类默认功放请求应成功");

    return 0;
}
