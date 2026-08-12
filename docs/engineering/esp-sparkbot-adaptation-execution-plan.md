# ESP-SparkBot 适配执行文档

> **本文件最初是本地执行草稿；根据当前授权，作为阶段性 Draft PR 提交。**
> 它仍不是最终实现或最终验收文档：正式实现必须继续拆成探针、显示、音频和交互等独立 PR；产品和架构定稿以对应 GitHub Issue 为准。

一句话结论：新板不是旧的 ESP32-S3 + SSD1306 + NoAudioCodec 组合，而是 `esp-sparkbot`：ESP32-S3、16MB Flash、8MB PSRAM、240x240 ST7789 彩屏、ES8311 双工音频、OV2640 摄像头和底盘 UART。适配必须从板级能力抽象开始，不能把 GPIO、显示和音频条件继续堆进 `runtime.cc`。

一句话动作：在 `feat/esp-sparkbot-adapter` 独立工作树中，先完成探针和能力矩阵，再按官方 SparkBot 原样显示、音频、交互、摄像头/底盘分阶段提交；PR #226 只提取可复用的视觉语义和动画思路，不能整体合并。

## 1. 执行基线

### 1.1 工作树和分支

```text
仓库：1024XEngineer/VoiceLife
基线：origin/main @ 0bc930d
工作树：/Users/mac/Desktop/project/.worktrees/voicelife-esp-sparkbot
分支：feat/esp-sparkbot-adapter
当前主工作树：/Users/mac/Desktop/project/VoiceLife（不改动）
```

主工作树中已有未跟踪目录 `output/`，适配工作不会清理、移动或覆盖它。新分支不建立在 PR #226、PR #230 或任何其他未合并分支上。

### 1.2 配网状态

新板串口为 `/dev/cu.usbmodem14101`，USB VID/PID 为 `303a:1001`，是 Espressif USB-JTAG/Serial，不是 CH340。

已完成的只写操作：

- 备份原始 NVS 到 `/tmp/voicelife-new-board-nvs-20260812.bin`；
- 将已提供的目标网络置于小智 NVS `wifi:ssid/password` 首项；
- 保留原网络为 `ssid1/password1` 回退项；
- 只写 `0x9000..0xCFFF` 的 16KB NVS 分区；
- 读回后通过 CRC 和命名空间校验，未修改 bootloader、分区表、factory、assets。

复位验证时已看到：

```text
Found AP: zxp, RSSI=-23, Channel=1, Authmode=3
WiFi connecting to zxp
Got IP: 10.103.199.73
Connected to WiFi: zxp
State: starting -> activating -> idle
```

后续 VoiceLife 固件的配网验收不能只看 NVS；必须在重启后看到 `WiFi connecting to zxp`、`Got IP` 和一次稳定的协议连接。日志和脚本都不得打印密码。

## 2. 已确认的硬件能力

| 能力 | 证据 | 适配要求 |
| --- | --- | --- |
| MCU | 启动日志：ESP32-S3 rev 0.2 | 使用 ESP-IDF ESP32-S3 构建目标 |
| Flash | `esptool flash_id`：16MB | 以实际分区表为准，不因容量增加而覆盖未知分区 |
| PSRAM | 启动日志：Found 8MB PSRAM | 图像缓存、LVGL 缓冲和音频队列优先放 PSRAM |
| 显示 | 小智 profile：ST7789 SPI、240x240 | 直接照搬小智官方 `esp-sparkbot` 实现，不能调用 SSD1306 初始化或自行改 UI |
| Codec | ES8311，日志显示 Duplex channels created | 使用双工 Codec Profile，不能沿用 NoAudioCodec simplex |
| 音频 GPIO | MCLK45、WS41、BCLK39、DIN40、DOUT42 | 在 `AudioHardware` 内声明，runtime 不直接写 GPIO |
| Codec I2C | SDA4、SCL5、ES8311 默认地址 | 启动阶段检查 I2C ACK 和 Codec reset |
| 功放/背光 | GPIO46 共用 | 建立单一 power-domain 仲裁；禁止 Codec 关闭影响背光 |
| BOOT | GPIO0 | 启动阶段配网；运行时按小智语义切换对话/打断 |
| 摄像头 | OV2640 PID `0x26`，DVP 初始化成功 | 仅用户请求时拍照；拍照不能阻塞语音任务 |
| 底盘 | UART1，TX38、RX48、115200 | 独立 `ChassisTransport`，命令有超时和停止策略 |
| 传感器 | 当前未发现 IMU、ToF、环境光、触摸 | 在探针中标记 absent/unknown，未确认前不承诺功能 |

官方参考资料：

- `/Users/mac/Desktop/project/xiaozhi-esp32/main/boards/espressif/esp-sparkbot/config.h`
- `/Users/mac/Desktop/project/xiaozhi-esp32/main/boards/espressif/esp-sparkbot/esp_sparkbot_board.cc`
- `/Users/mac/Desktop/project/xiaozhi-esp32/main/boards/espressif/esp-sparkbot/config.json`

## 3. 产品范围和取舍

### P0：先把板变成可靠的 VoiceLife 终端

- Wi-Fi 首选、回退、重新配网和自动重连；
- ES8311 录音、TTS 播放、本地唤醒/告别提示音；
- 官方 SparkBot 的 ST7789 方向、颜色、刷新、状态动画和文本显示，禁止自行改动显示方案；
- BOOT 按键的唤醒、打断、配网语义；
- `Idle -> Listening -> Finalizing -> Thinking -> Speaking -> Followup/Idle` 完整回合；
- 灯光和功放在各状态下的明确开关状态；
- 启动、断网、服务端 goodbye、异常断开等日志闭环。

### P1：板载能力带来的自然交互

- 用户明确要求时拍照并回写视觉结果；
- 摄像头翻转配置；
- 底盘灯光作为短暂状态反馈；
- 用户确认后才允许移动控制。

### P2：暂不承诺

- IMU 拿起/翻转/摇动交互；
- 环境光自动亮度；
- ToF 靠近唤醒；
- 触摸手势。

这些功能只有在原理图、BOM 或实板探针确认硬件存在后才能进入范围。

## 4. 目标架构

```text
VoiceLifeRuntime
  -> VoiceTurnCoordinator
      -> LinxSpeechProvider
      -> AudioHardware
      -> StatusDisplay
      -> BoardControls
      -> SensorHub

BoardProfile
  -> SparkBotProfile
      -> SparkBotEs8311Audio
      -> SparkBotSt7789Display
      -> SparkBotCamera
      -> SparkBotChassisUart
```

### 4.1 板级契约

建议新增以下公开模型，不让上层接触 GPIO：

```text
BoardProfile
  board_id / sku
  display: DisplayCapabilities
  audio: AudioCapabilities
  controls: ControlCapabilities
  sensors: SensorCapabilities
  power: PowerDomainCapabilities

DisplaySnapshot
  phase
  emotion
  status_text
  user_text
  assistant_text
  time_text
  animation_id
  scroll_offset

AudioHardware
  configure(format)
  start_capture()/stop_capture()
  push_playback(frame)
  drain_playback(timeout)
  set_output_enabled(enabled)
```

### 4.2 显示渲染契约

**硬约束：SparkBot 屏幕显示方案直接照搬小智官方实现，不做任何修改。**

- 直接移植小智仓库 `main/boards/espressif/esp-sparkbot/`、`main/display/lcd_display.cc` 和 `main/display/lvgl_display/` 中的 ST7789/LVGL 初始化、布局、主题、动画资源、状态映射、字体、字号、颜色、滚动和刷新节奏；
- 不把 VoiceLife 旧板的 SSD1306 牛头布局移植为新板界面；
- 不重新设计状态栏、文本区域、动画时序或屏幕交互；
- VoiceLife 只负责把语音会话状态转换为小智 SparkBot 显示实现已有的状态/文本输入，显示层不得反向改变会话状态；
- 动画只由显示任务刷新，禁止在音频 DeliveryLoop、WSS 回调或 Provider 回调中直接操作 LVGL；
- 只有官方 SparkBot 显示方案无法编译或无法在实板工作时，才记录阻塞证据并暂停，不能自行发明替代 UI。

## 5. 分阶段执行

### 阶段 A：板级探针和能力矩阵

目标：先证明硬件边界，再写适配代码。

任务：

1. 建立 SparkBot `BoardProfile` 空骨架；
2. 输出启动探针：芯片、Flash、PSRAM、分区、SKU、MAC 脱敏值；
3. I2C 扫描 GPIO4/5，确认 ES8311 地址和复位；
4. 初始化 ST7789，确认 SPI mode、方向、偏移和 240x240 可视区域；
5. 初始化 ES8311，输出输入/输出采样率和 PA 状态；
6. OV2640 探测和单帧拍照；
7. UART1 回环/底盘停止命令探针；
8. BOOT 按键和 GPIO46 power-domain 探针；
9. 形成 `hardware matrix`：每一项标记 `verified`、`absent` 或 `needs-board-test`。

验收：探针可重复运行三次，日志没有重复初始化、I2C 冲突或 GPIO46 抖动。

建议提交：

```text
feat(board): add SparkBot hardware profile and probe
```

### 阶段 B：显示驱动和动画

目标：直接移植小智官方 SparkBot 显示代码和资源，让 VoiceLife 状态接入既有显示输入，不影响音频。

任务：

1. 直接移植小智官方 SparkBot 的 ST7789/LVGL Renderer 代码；
2. 直接移植小智官方 `boot/connecting/idle/listening/thinking/speaking/error` 动画资源；
3. 直接移植小智官方状态栏、文本布局、字体、字号、标点、滚动和刷新时序；
4. 仅建立 VoiceLife 状态到小智 SparkBot 显示输入的适配映射，不改显示方案本身；
5. 确认 LVGL tick、flush 和动画缓存不会运行在音频任务栈上；
6. 开机、联网、空闲、聆听、处理中、播报、告别、错误逐项截图留档，并与官方 SparkBot 参考行为比对。

建议提交：

```text
feat(display): add SparkBot ST7789/LVGL renderer
```

### 阶段 C：ES8311 音频、功放和提示音

目标：先让本地 1kHz 探针和提示音可靠，再接云端 TTS。

任务：

1. 实现 SparkBot ES8311 duplex profile；
2. 把 GPIO46 的背光/功放控制集中到 power arbiter；
3. 统一 Provider hello、PCM frame 和 I2S/Codec 格式；
4. 本地提示音使用协商后的输出格式或明确的重采样路径；
5. 播放队列使用 PSRAM，增加队列深度、拒绝原因、背压和 drain 日志；
6. 唤醒提示音在开麦前排空，告别提示音在会话彻底收尾后播放；
7. 验证 `audio_played`、`audio_rejected`、实际 I2S 写入和 Codec 输出使能一致；
8. 录音回路验证 `capture_started -> audio_sent -> stt_text_received`。

建议提交：

```text
feat(audio): add SparkBot ES8311 duplex path
```

### 阶段 D：回合状态、按钮和联网

目标：解决“第一次慢、牛牛走了覆盖回复、无法二次唤醒、多次联网”等历史问题。

任务：

1. 引入单一 `VoiceTurnCoordinator` 和 generation token；
2. VAD 端点后进入 `Finalizing`，发送 `listen.stop`，等待最终 STT；
3. 最终 STT 到达后进入 `Thinking`，再接受同一回合的 TTS；
4. TTS 播放排空后按策略进入 Followup，不立即回 Idle；
5. 服务端正常 goodbye 与异常断开分流；
6. BOOT 单击、长按、打断和配网行为对齐小智；
7. 音量按键只调音量，不触发会话复位；
8. 联网只允许一个 owner，禁止 Runtime、Provider、Transport 各自重连；
9. 每次重连生成新的 transport generation，旧事件全部丢弃；
10. 待机时功放关闭、灯光关闭、屏幕显示时间或 idle 动画。

建议提交：

```text
fix(runtime): close SparkBot voice turns and power states
```

### 阶段 E：摄像头和底盘能力

目标：在 P0 稳定后再增加有趣功能。

任务：

1. 增加 `CameraCapability` 和按需拍照 API；
2. 拍照时暂停或降低语音资源竞争，不阻塞音频播放任务；
3. 增加 `ChassisTransport`，命令带序列号、超时、急停；
4. 移动命令必须有用户确认和停止命令；
5. 灯光模式只作为短暂反馈，待机自动回到关闭状态；
6. 摄像头和运动能力默认关闭远程高风险操作。

建议提交：

```text
feat(board): add SparkBot camera and chassis capabilities
```

## 6. PR #226 的替代流程

PR #226 当前是 Open、非 Draft、`mergeable=CONFLICTING`、`mergeStateStatus=DIRTY`，没有 Milestone、assignee 或 reviewer。

处理顺序：

1. 在新板适配 Issue 中记录 PR #226 的提取清单；
2. 新 PR 只提交当前阶段一个交付单元，不能 cherry-pick 整个 #226；
3. 在 #226 留下替代 PR 链接和关闭原因；
4. 确认新 PR 具备 Issue 引用、验收、测试和 reviewer 后关闭 #226，不合并；
5. 新板最终 PR 才使用 `Fixes #新Issue`，探针、架构和中间 PR 使用 `Refs #新Issue`。

可提取：牛头视觉语义、表情命名、状态映射、UTF-8 边界处理、动画测试思路。

不可提取：旧 OLED 布局、少量手写中文字形、阻塞式 wake callback、忽略 `fin` 的 WebSocket 重组、协议/音频/状态机/显示混合提交。

## 7. 测试和验收

### 主机测试

- `audio_board_profile_contract_test`
- `voice_session_contract_test`
- `voice_interaction_controller_test`
- Linx codec/provider/transport contract tests
- 新增 `sparkbot_profile_contract_test`
- 新增 DisplaySnapshot/Renderer model test

### 实板测试矩阵

| 场景 | 必须看到的证据 |
| --- | --- |
| 冷启动 | 一次 Wi-Fi 初始化、一次协议连接、一次显示初始化 |
| 配网 | AP 进入/提交/退出，凭据不出现在日志 |
| 唤醒 | WakeAck 音频和动画先完成，再进入 Listening |
| 录音 | capture_started 后持续发送音频，STT 在同一 generation 到达 |
| VAD 收尾 | listen.stop 后进入 Finalizing，承接最终 STT |
| TTS | Speaking、I2S 写入、audio_played 同步，audio_rejected 不增长 |
| Followup | 回复排空后保持聆听，超时后一次告别并回 Idle |
| goodbye | 服务端正常关闭不显示错误 |
| 断网 | 只启动一个重连流程，不出现联网循环 |
| 屏幕 | 240x240 方向、布局、动画、字体、字号、长文本和英文标点与小智官方 SparkBot 实现完全一致 |
| 摄像头 | 单帧拍照成功，语音任务不死锁 |
| 底盘 | 前进/后退/转向/急停均有超时和日志 |
| 稳定性 | 20 轮连续对话、5 次快速唤醒、3 次断网恢复 |

### 回滚

- 保留当前官方 SparkBot factory 固件；
- 保留原始 NVS 备份；
- 适配阶段只允许写目标 OTA app 和明确的板级数据分区；
- 禁止覆盖 bootloader、分区表、factory、assets 和未知数据分区；
- 每次实板测试前记录当前启动槽、分区表和固件 SHA256。

## 8. 当前阻塞和下一步

当前可以立即开始的是阶段 A：板级 Profile 和探针。以下事项必须先拿到证据：

- VoiceLife 目标分区表和适配固件镜像；
- ST7789 实际 SPI 偏移、旋转和背光极性；
- ES8311 Codec/PA 在目标 VoiceLife 构建中的实际输出；
- 摄像头拍照是否纳入 P0；
- 底盘 UART 协议是否允许 VoiceLife 控制；
- 仓库目标 Milestone 和负责 Review 的人。

执行顺序固定为：

```text
Issue/Proposal
 -> Board probe
 -> Architecture skeleton
 -> Display PR
 -> Audio PR
 -> Runtime/interaction PR
 -> Camera/chassis PR
 -> 20-round hardware acceptance
 -> final PR / close #226
```

## 9. 文档留存规则

本文件作为本阶段计划 Draft PR 的唯一文件提交，但不代表实现已经完成：

- 本 PR 只记录执行计划和当前已验证事实；
- 不在本 PR 中混入运行时代码、固件、分区表或凭据；
- 不修改或关闭 PR #226；
- 正式产品/架构定稿仍回写到新建的 Proposal/Design Issue；
- 后续实现按阶段创建独立 PR，并在最终 PR 满足验收后使用 `Fixes #新Issue`。
