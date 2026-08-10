# Linx MVP 实现变更记录

这份记录说明 2026-08-10 对 Linx MVP 计划做了哪些代码落地，以及哪些验收还不能算完成。结论：当前板已真实联网、执行 Linx OTA，并稳定完成 WSS hello 与服务端 MCP 初始化消息；STT/TTS 真机闭环仍需真实人声验收，不能用静音或 mock 替代。

## 已落地

- `voicelife_linx_esp` 的公共 transport 文件只保留包装层，拆分后的 `Impl` 成为唯一实现，消除了重复定义风险。
- ESP Runtime 注册 `xrobot-websocket` Provider，连接 `LinxJsonCodec`、`EspWebSocketTransport` 和 `LinxSpeechProviderAdapter`。
- Runtime 从 Linx OTA 响应生成当前连接配置；ESP Transport 只通过受控 NVS 引用解析 token，不把 token 放入 Runtime 配置或日志。
- `esp32s3-voicelife-pcb-pcm` Profile 已将 speech adapter 声明为 `xrobot-websocket`，并启用独立 HMAC 加密 NVS；默认 Wi-Fi NVS 保持关闭，避免触碰旧 `nvs`。
- token 只接受 `nvs://namespace/key` 引用；解析失败会返回明确错误，不打印 token，也不自动擦除 NVS。
- ESP 音频路径使用 `VoiceLifePcbEsp32s3Profile()` 的 PCM/I2S 输入输出；host 继续使用 scaffold，保持现有 smoke 测试契约。
- Runtime 已注册 `schedule.create` 和 `schedule.query`；两者使用 Unix 秒时间参数，调用现有 `ScheduleService`，不把 mock 数据描述成真实持久化。
- Runtime 启动时输出仅含工具名称和数量的 MCP 就绪标记；VoiceSession 输出不含详情正文的生命周期证据，避免串口记录用户语音、Wi-Fi 或凭据。
- 真实 ESP 路径在音频端口和可选硬件 smoke 完成后调用 `VoiceSession::BeginCapture()`，使用 realtime 模式开始上行 PCM；host 仍保持 scaffold smoke。
- hello 声明 MCP 能力；Linx `type=mcp` 消息会由 Runtime 处理 JSON-RPC `initialize`、`tools/list` 和 `tools/call`，并通过同一 WebSocket 回传结果。
- 架构门禁显式登记 Runtime 的 `nvs_flash` 依赖。
- 新增 `voicelife_linx` OTA 协议层：严格构建官方要求的 HTTPS POST header、设备/板型/Wi-Fi/分区请求体，并解析 activation、websocket、server_time 与 firmware 响应。`websocket.token` 仅在服务端明确返回时保留；无 token 的文档示例不能生成 WSS 连接配置。
- ESP Runtime 新增仅在加密 NVS 前提下工作的预置凭据 STA 生命周期：从 `wifi/ssid`、`wifi/password` 读取配置，初始化 netif、事件循环和 Wi-Fi，并在获得 IP 后才继续 OTA；未启用 NVS encryption 或未配置凭据时安全失败。
- 重新直接读取当前串口板后，修正 `esp32s3-voicelife-pcb-pcm` 的板级分区基线：物理 Flash 仍为 16 MB，但当前安装固件实际分区为 `nvs@0x9000/0x6000`、`otadata@0xf000/0x2000`、`phy_init@0x11000/0x1000`、`ota_0@0x20000/0x3e0000`、`ota_1@0x400000/0x3e0000`、`voicelife@0x7e0000/0x200000`，没有 `assets` 或 `nvs_keys`。后续只允许按这份实测布局写入 `ota_1`，原先拟定的 16 MB CSV 不适用于当前板。
- 依据 Linx 官方 WebSocket 文档，PCM hello 正式支持 `format=pcm`、16 kHz/单声道/16-bit；Runtime 仍拒绝服务端协商出的 Opus，因为当前板级没有 Opus decoder。PCM 播放 I2S 已改为使用服务端协商的采样率和帧时长，支持 16 kHz 或 24 kHz PCM。
- Linx 服务端 hello 的 `session_id` 由 Provider 记录并用于后续 `listen`、`abort` 和 `detect`；后续显式携带不同 session 的下行消息会被拒绝。服务端未提供该可选字段时维持兼容，不把本地 Runtime 会话名误当成 Linx 服务端会话。
- ESP-IDF WebSocket 客户端的 `websocket_task` 栈通过公开的 `esp_websocket_client_config_t::task_stack` 配置为 12288 字节；这修复了实板上观察到的 `websocket_task` stack overflow。自有事件 worker 仍单独配置栈，两个任务不共用容量参数。
- ESP-IDF 会将 Ping、Pong、Close 控制帧以 `WEBSOCKET_EVENT_DATA` 回调；Transport 现在只将 text、binary、continuation 数据帧送入 Linx assembler，控制帧由 managed client 自行处理，避免误报 Provider 协议错误。
- 无 AEC 的板型在收到 Linx `tts:start` 时会先停止 I2S 采集，再进入播放状态；host 契约验证随后二进制 PCM 帧可进入输出端口，`tts:stop` 后会话回到 ready。
- Runtime 在 capture、STT、MCP、TTS 和失败生命周期点输出脱敏验收指标：从采集开始的延迟、PCM 采集/丢弃/播放/拒绝帧数与最小空闲堆。事件详情仍不进入串口，因此不记录语音文本或 MCP 参数。
- `scripts/collect_linx_e2e_evidence.py` 只采集上述 allowlist 指标，并按场景标签写入 JSON；它不保存原始串口行、设备标识、凭据、语音文本或 MCP 业务参数，供最后真实人声验收使用。

## 证据

### 2026-08-10 当前板受控验证

- `flash_id` 直接读取到 ESP32-S3 QFN56 revision v0.2、16 MB Flash、8 MB Embedded PSRAM。设备唯一标识不记录到文档。这确认当前连接板是教程 PCB 版对应的 N16R8 硬件；“只使用约 8 MB”来自旧固件分区表，不是物理 Flash 容量。
- 115200 只读备份保存在仓库外 `/tmp/voicelife-linx-baseline-20260810-actual/`，包括真实分区表、`nvs`、`otadata`、`ota_0`、`ota_1`、`voicelife`，每项均完成字节数检查、SHA-256 和 esptool 回读校验；没有 `assets` 分区。
- HMAC NVS 安全基线已执行：`BLOCK_KEY0` 写入随机 key，`KEY_PURPOSE_0=HMAC_UP`，默认读/写保护已启用；没有开启 Flash Encryption 或 Secure Boot。临时 key 文件已覆盖删除，未进入仓库或日志。
- 新 Profile 镜像仅写入实测 `ota_1@0x400000` 并完成 esptool 回读校验；受控启动日志确认 HMAC NVS provider 注册、MCP 工具注册、Wi-Fi 获得 IP，并进入 Runtime 主干。
- 首次联网后发现 `PartitionTableJson()` 对 ESP-IDF 迭代器进行了重复释放，导致 TLSF double-free；修复为遵守 `esp_partition_next()` 的释放契约后，重复刷写验证不再出现该断言。
- OTA HTTPS 请求已真实执行；本轮只记录响应结构存在性，不记录 URL、token、激活码或 Wi-Fi 数据。通过官方固定主机的 `ws://` -> `wss://` 升级后，实板已完成 TLS WebSocket 连接、hello 音频协商和服务端 MCP 初始化/工具列表消息。
- 实板最初每约 10 秒出现一次 provider error。回溯 ESP-IDF 源码确认 Ping/Pong 控制帧被错误送入业务 assembler；修复后，在跨越多个保活周期的同一 WSS 连接窗口内保持 ready，未再出现 provider error、事件队列溢出或栈溢出。
- 当前没有可审计的 STT 文本、MCP `tools/call` 成功或 TTS 播放证据，因此不能宣称闭环完成。

- `./scripts/run_checks.sh`：通过，41/41 主机测试、公共 API、架构边界、双端契约和 Python 测试均通过。
- `python3 scripts/firmware.py validate`：通过三个固件 Profile 校验。
- `clang-format --dry-run --Werror`、`git diff --check`：通过。
- 官方 WebSocket 协议复核后补充了服务端 session 绑定主机契约：服务端分配的 ID 会出现在后续控制消息，其他 session 的下行消息被拒绝。
- 已在 ESP-IDF 6.0.2 下构建 `esp32s3-voicelife-pcb-pcm`：本轮 `voicelife.bin` 为 `0x170bf0`，最小 App 分区余量约 63%。
- 本轮向实测非活动 `ota_1@0x400000` 写入并验证了测试 App，并用 `otatool` 切换到该槽；随后已恢复原始 `ota_1` 与 `otadata`，并以 esptool digest 回读校验。
- 直接读取板上 eFuse 的安全状态显示 Secure Boot 与 Flash Encryption 仍未启用；经用户批准，`BLOCK_KEY0` 已配置为 HMAC NVS 用途并启用读写保护，密钥内容未读取、未进入仓库或日志。
- OTA 返回待激活或缺少 WSS 配置时只输出脱敏状态，不输出 activation code、challenge、token 或 Wi-Fi；完成控制台绑定仍需真实 Linx 控制台操作。
- 原生 MVP 的历史协议脚本以 `format=pcm`、16 kHz、单声道、16-bit 发起 hello，保存的 manifest 记录了真实 OTA/WSS、STT、MCP 和 TTS 二进制下行。这是 PCM 上行与完整服务链曾被 Linx 接受的迁移证据，不是当前板或当前凭据的验收。脚本只把服务端 hello 标为已收到，并把任意二进制累计为 TTS 字节数，未保存服务端 `audio_params`，也未验证下行二进制的编码；因此不能据此断言 Linx 会协商 PCM 下行或排除 Opus。

## 未完成与下一步

Linx 官方 WebSocket 文档同时描述 PCM hello 和 Opus 下行；当前 `Esp32s3PcmAudioPorts` 支持协商后的 PCM 16/24 kHz，但没有 Opus decoder。`LinxSpeechProviderAdapter` 会在 hello 协商把编码改为 Opus 时失败，避免把压缩数据当 PCM 播放。这使“WSS hello”与“语音到 TTS 播放”仍必须拆成两个验收步骤。

1. 最后以真实人声完成至少各 3 次创建与查询，记录脱敏后的 STT、MCP `tools/call`、TTS 生命周期、延迟、最低空闲堆及帧丢弃/拒绝计数。当前测试板没有可验证的人声输入，不能把静音连接当成此项通过。
2. 记录脱敏后的服务端 `audio_params` 并验证实际采样率、帧长和 PCM 播放；若为 Opus，再按许可证、Flash/RAM 预算单独设计解码器。历史 PCM 脚本不能替代这一步。
3. 完成凭据安全存储与用户配网。已实现仅在加密 NVS 前提下工作的预置 STA 启动；当前 Profile 已启用 HMAC NVS 配置，板上已完成 HMAC eFuse 基线，Wi-Fi 凭据通过一次性物理串口协议写入独立加密分区，不把 Wi-Fi 或 Linx token 写入普通 NVS。
4. 只有前述条件满足后，采集真实的 STT、MCP `tools/call`、TTS 和音频播放证据；当前不宣称真机闭环完成。
