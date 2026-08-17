# voicelife_display_sparkbot

ESP-SparkBot 彩屏（ST7789/LVGL）显示实现组件，直接移植自小智官方
SparkBot 显示实现（唯一实现来源）。

## 移植来源

上游基线：`xiaozhi-esp32` commit `37d1aee793f99a9b865957acc3798d06335c3ad0`。

| 文件 | 来源 | 说明 |
| --- | --- | --- |
| `src/sparkbot_lvgl_display.cc` | `main/display/lcd_display.cc`（SpiLcdDisplay）+ `main/boards/espressif/esp-sparkbot/esp_sparkbot_board.cc`（InitializeSpi/InitializeDisplay） | ST7789/LVGL 初始化 |
| `src/sparkbot_lvgl_renderer.cc` | `main/display/lcd_display.cc`（SetupUI 简单模式） | 官方简单模式 UI 与状态映射 |
| `src/sparkbot_emoji_assets.cc` | `main/assets.cc`（LvglStrategy） | assets 分区 mmap 解析 |
| `src/gif/gifdec.{h,c}` | `main/display/lvgl_display/gif/gifdec.{h,c}`（vendored） | GIF 软件解码 |
| `src/gif/lvgl_gif.{h,cc}` | `main/display/lvgl_display/gif/lvgl_gif.{h,cc}` | LVGL GIF 播放控制器 |

## Vendored 代码许可

- `gifdec.{h,c}`：上游为 <https://github.com/lecram/gifdec>（MIT 许可），经
  xiaozhi-esp32 适配。文件头部已注明 vendored 来源；仅按上游维护，不得视为
  VoiceLife 原创代码。
- 其余文件为 xiaozhi-esp32（MIT 许可，Copyright Shenzhen Xinzhi Future
  Technology Co., Ltd.）的移植/适配，见 `assets/esp-sparkbot/manifest.json` 的
  source 记录。

## 资源加载约束

- 资源只允许通过受控 `asset_id`（与 manifest 一致）加载；
- 不接受 URL、任意文件路径或任意字节流；
- 不做运行时网络下载；资源随固件 assets 分区提供。

## 当前状态

- ST7789/LVGL 初始化与官方简单模式 Renderer 已移植（编译级）；
- 实板显示未验证：`available=false`，`SparkBotPresentationAdapter` 的
  Render/Submit 在实板验证前保持 `kUnavailable`；
- emoji GIF 播放（gifdec + lvgl_gif）已移植，接入 Renderer 待实板验证阶段。
