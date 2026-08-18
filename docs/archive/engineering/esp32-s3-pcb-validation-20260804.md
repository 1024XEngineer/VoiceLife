# 历史归档：ESP32-S3 PCB 实板变更与恢复（2026-08-04）

> 本记录只说明当时 PCB、分区和验证结果，不能直接套用到当前板卡。当前日志规则见[硬件调试与串口日志](../../engineering/hardware-debugging.md)，当前 SQLite 恢复操作见[SQLite 实板验证与恢复](../../engineering/board-storage-validation.md)；归档原因见 Issue [#264](https://github.com/1024XEngineer/VoiceLife/issues/264)。

这份守则把一次真实断流事故变成固定流程：关键读取与写入统一使用已经验证稳定的 115200，测试固件只进入非活动 OTA 槽，任何切换前都要能恢复原数据分区和 OTA 元数据。#111 的纯 I2S 验证也把 PCM 削波比例列为必留证据。
下一步动作：涉及分区、烧录、OTA 或数据库实测的 PR，必须按本文留下脱敏的尺寸、哈希、启动槽和恢复结果；缺一项就不能把“固件可构建”写成“实板可用”。

## 1. 已确认的板级事实

当前主验证板是立创实战派 ESP32-S3，实测为 16 MB Flash、8 MB PSRAM，运行在 `ota_0`。板上不是新工程默认的单工厂分区，而是双 OTA 加独立数据区：

| 分区 | 地址 | 大小 | 处理原则 |
| --- | ---: | ---: | --- |
| `nvs` | `0x009000` | `0x004000` | 只做加密/脱敏备份，不输出内容 |
| `otadata` | `0x00d000` | `0x002000` | 切换测试槽前备份，测试结束恢复 |
| `ota_0` | `0x020000` | `0x3f0000` | 当前运行槽，不覆盖 |
| `ota_1` | `0x410000` | `0x3f0000` | 唯一允许写入测试 App 的槽 |
| `assets` | `0x800000` | `0x600000` | 运行数据，测试期间保持不变 |
| `voicelife` | `0xe00000` | `0x200000` | SQLite/业务数据，先完整备份再测试 |

新工程当前的默认刷写地址是 bootloader `0x0`、partition table `0x8000`、App `0x10000`。它与这块板的双 OTA 布局不兼容，禁止直接执行全量 `flash` 或使用默认 `flasher_args.json` 覆盖整板。

## 2. 为什么固定 115200

同一块板在 460800/921600 读取大分区时出现过随机断流，115200 完整读取 2 MB `voicelife` 分区则没有中断。关键操作以可重复为先：快几分钟不值得换一次不可判断的半份备份。

- 设备身份探针、分区表、NVS、OTA 元数据和业务数据统一使用 115200。
- 读取完成后同时检查进程退出码、文件字节数和 SHA-256；只有“命令没报错”不算备份成功。
- 串口断开、长度不符或哈希复读不一致时立即停止，不继续写入。
- 非活动 OTA 槽只保存程序镜像，不包含 NVS、SQLite、assets 或当前 OTA 选择；备份 `ota_1` 不能代替数据分区备份。

## 3. 固定操作顺序

1. 记录串口、芯片 revision、Flash/PSRAM、晶振和当前启动槽。
2. 读取并解析真实分区表，不从仓库默认 CSV 猜板上布局。
3. 以 115200 备份 `partition-table`、`nvs`、`otadata` 和本次测试会触及的数据分区。
4. 校验每个备份的预期字节数与 SHA-256；备份保存在仓库外，不提交 Git，不上传 Issue。
5. 构建与真实分区兼容的 App，确认镜像小于 `0x3f0000`，只写 `ota_1@0x410000`。
6. 通过受控 OTA 元数据临时切换到 `ota_1`，以 115200 记录启动、最低空闲堆和测试证据。
7. 测试失败先保存脱敏日志，不反复提速或扩大写入范围。
8. 恢复原 `otadata`，确认重新从 `ota_0` 启动；复核 NVS、assets 和 `voicelife` 分区未被改变。

## 4. SQLite 与语音测试的额外约束

- SQLite 事务测试只操作 `voicelife` 分区，先做完整镜像备份；不得用 OTA 槽代替数据库备份。
- 音频任务不直接持有 SQLite 连接。实测事务延迟已经远超 10/20 ms 音频帧预算，数据库写入必须经过业务队列。
- WSS、hello 或音频闭环没有真实凭据时，明确记录“未执行”；不能用 echo、mock 或启动日志代替云端通过。
- 凭据只从 GitHub Secrets、NVS 或 `secret://` 引用进入运行时。串口日志、备份文件名和命令行不得出现 token、AK、SK 或用户数据。

## 5. #109 Lichuang Profile 受控启动验证

2026-08-04 在 `/dev/cu.usbmodem5A840116301` 以 115200 完成一次可回退验证。原固件日志报告 `SKU=voicelife-pcb`、`NoAudioCodec`，其 GPIO/拓扑与小智 `bread-compact-wifi` Profile 一致；这不是 Lichuang ES8311/ES7210 Codec 板：

- 设备为 ESP32-S3 QFN56 revision v0.2，16 MB Flash、8 MB Embedded PSRAM；真实分区表与既有快照一致。
- 构建产物 `voicelife.bin` 为 222320 bytes（`0x36470`，SHA-256 `79ec0f3d81a622a24a4484943efe823665bd4ce5739ad8ec0b27671b7eb7f1c4`），只写入 `ota_1@0x410000`，未覆盖 bootloader、分区表、`nvs`、`assets` 或 `voicelife`。
- OTA 镜像回读 222320 B，与构建产物逐字节一致；`otadata` 写入前哈希为 `8ba3b110139f45443d4f268d1a3373ef99a1718b71d51664531b83ee2d4b91a3`，恢复后逐字节一致。
- 新固件从 `ota_1` 真实启动，串口确认 `VoiceLifeRuntime: 音频探针`、`VoiceLife 架构主干已启动`、`I2S_READY=1`、`I2S_STARTED=1`、`write=480`、`read=480`。
- 同一串口日志确认 `ES8311=0`、`ES7210=0`、`PCA9557=0`；这符合当前 `bread-compact-wifi` 的 `NoAudioCodec` 板型，不构成 Lichuang Codec 通过证据。
- 测试结束恢复原 `otadata`，再次启动确认原固件 `xiaozhi 2.4.0` 从 `ota_0` 运行；SQLite 数据仍可加载 7 个事件、8 个提醒、0 条笔记。
- 本次证明了分区兼容、写入校验、启动恢复和纯 I2S DMA smoke；没有把启动日志当成 WSS、ASR、TTS、Codec 录放或物理音频闭环证据。
- 本次启动 smoke 未采集最低空闲堆统计；该项随真实 Transport/音频链路接入补测。

## 6. #111 voicelife-pcb 纯 I2S PCM 验证

2026-08-04 在同一串口以 115200 完成两轮 `esp32s3-voicelife-pcb-pcm` 对照，板型仍为原固件报告的 `SKU=voicelife-pcb` / `NoAudioCodec`。Profile 依据旧 MVP 的 `NoAudioCodecSimplex` 保留 I2S1 麦克风和 I2S0 扬声器拓扑，但把采集 PCM 对齐从 `>>12` 调整为 `>>14`：

| 端点 | controller | 采样率 | GPIO | wire slot | PCM 对齐 |
| --- | --- | ---: | --- | ---: | ---: |
| capture | I2S1 RX | 16 kHz | `SCK=5 / WS=4 / DIN=6` | 32 bit | 右移 14 |
| playback | I2S0 TX | 24 kHz | `BCLK=15 / LRCK=16 / DOUT=7` | 32 bit | 左移 16 |

采用 `>>14` 的最终镜像为 `229488` bytes（`0x38070`），SHA-256 为 `c154342f93e290f22a2260fcc1d994445ede91a66db6bb1f44f1d86cee623c32`；只写入 `ota_1@0x410000`，回读同样为 `229488` bytes 且逐字节一致。启动日志确认 `I2S_READY=1`、`I2S_STARTED=1`、`profile=esp32s3-voicelife-pcb-pcm`，指标如下：

- `pcm_samples=4800`、`bus_read=19200`、`bus_write=960`、`replay=960`；
- `nonzero=4800`、`changed=4716`、`saturated=1`、`saturation_ppm=208`；
- `peak=32767`、`mean_square=45611365`、`signal=1`、`min_heap=369528`。

旧 MVP 的 `>>12` 对照结果为 `saturated=383`、`saturation_ppm=79791`、`mean_square=206790038`。两轮都保持 `changed` 很高，因此 `>>14` 不是把输入压成静音，而是给后续 DSP 留出有效余量。该结论只适用于本板这次 PCM 探针，新增板卡仍需重新测量。

这次通过的是数字证据：I2S DMA、PCM 非零/变化、削波比例和有限总线回放。没有外部麦克风、示波器或人工听感记录，因此不能宣称 `physical_playback_verified=true`，也没有验证 ES8311/ES7210/PCA9557、AFE、AEC、WakeNet、Opus、WSS、ASR 或 TTS。

测试结束后写回原始 `otadata`，回读 SHA-256 为 `8ba3b110139f45443d4f268d1a3373ef99a1718b71d51664531b83ee2d4b91a3`，与备份逐字节一致；复位日志确认原固件从 `ota_0` 启动，`VoiceLifeStorage` 仍加载 `7 events, 8 reminders, 0 notes`。`nvs`、`assets` 和 `voicelife` 分区没有写入。

ESP-IDF 6.0.2 的 `otatool.py` 在其配套 Python 环境（esptool 5.3.1）中可以完成 `switch_ota_partition`；本机系统 esptool 4.12 仍使用下划线子命令，因此显式地址写入和回读采用下列稳定回退路径：

```bash
python -m esptool --chip esp32s3 -p /dev/cu.usbmodemXXXX -b 115200 \
  write_flash 0x410000 build/esp32s3-voicelife-pcb-pcm/voicelife.bin
python -m esptool --chip esp32s3 -p /dev/cu.usbmodemXXXX -b 115200 \
  read_flash 0x410000 0x38070 /tmp/voicelife-ota1-readback.bin
shasum -a 256 /tmp/voicelife-ota1-readback.bin
```

将回读文件与构建产物逐字节比对后再切换 `otadata`；不要因此改用 460800/921600，也不要把回退命令扩展成全量刷写。

## 7. PR 最低证据

硬件 PR 至少写清板卡、固件 SHA-256、App 大小、目标槽、串口波特率、测试步骤、PCM 样本/非零/变化/削波/均方值、最低空闲堆和恢复结果。失败同样是有效证据；真正不能接受的是没有恢复路径的“再试一次”。

## 8. #113 PCM Audio Port 验证

2026-08-04 在 `/dev/cu.usbmodem5A840116301` 以 115200 验证 Profile 驱动 Audio Port。写入前重新读取板上分区表，并完整备份 `nvs` 16 KiB、`otadata` 8 KiB 和 `voicelife` 2 MiB；`voicelife` 备份 SHA-256 为 `b00a74d6a87f5376d027a4f7665989934198ab9706689f287a545b4e18daaf72`。

测试镜像为 249952 B（`0x3d060`），SHA-256 `6ca07655c4a218b56967e7a074335817708e3ea8ada154fa9df299160964cdb7`。镜像只写入 `ota_1@0x410000`，回读 249952 B 后哈希和逐字节比较均一致。

启动日志：

```text
AUDIO_PORT_READY=1
AUDIO_PORT_CAPTURE_FRAMES=4
AUDIO_PORT_PLAYED_FRAMES=1
AUDIO_PORT_DROPPED_INPUT=0
AUDIO_PORT_REJECTED_OUTPUT=0
AUDIO_PORT_SHORT_READS=0
AUDIO_PORT_SHORT_WRITES=0
AUDIO_PORT_MIN_HEAP=358016
AUDIO_PORT_SIGNAL=1
```

同次底层探针记录 `pcm_samples=4800`、`nonzero=4797`、`changed=4487`、`saturated=228`、`saturation_ppm=47500`。这次验证证明 10 ms I2S period、60 ms PCM 组帧、有界队列和总线回放任务可用；较高削波需要受控声源复测，不能宣称声学质量已通过。

结束后恢复原 `otadata`，恢复前后 SHA-256 均为 `8ba3b110139f45443d4f268d1a3373ef99a1718b71d51664531b83ee2d4b91a3`。复位日志确认原固件从 `ota_0` 启动，SQLite 仍加载 7 个事件、8 个提醒、0 条笔记。
