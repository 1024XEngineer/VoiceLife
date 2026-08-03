# ESP32-S3 实板变更与恢复

这份守则把一次真实断流事故变成固定流程：关键读取与写入统一使用已经验证稳定的 115200，测试固件只进入非活动 OTA 槽，任何切换前都要能恢复原数据分区和 OTA 元数据。
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

## 5. 已完成的 ESP32-S3 受控启动验证

2026-08-04 在 `/dev/cu.usbmodem5A840116301` 以 115200 完成一次可回退验证：

- 设备为 ESP32-S3 QFN56 revision v0.2，16 MB Flash、8 MB Embedded PSRAM；真实分区表与既有快照一致。
- 构建产物 `voicelife.bin` 为 172480 bytes（`0x2a1c0`，SHA-256 `6ecbd3c32ab3ba13817ebf68acbc790141d4fa65616d85d283331fac68892c69`），只写入 `ota_1@0x410000`，未覆盖 bootloader、分区表、`nvs`、`assets` 或 `voicelife`。
- 新固件从 `ota_1` 真实启动，串口确认 `Project name: voicelife`、`App version: 0.1.0` 和 Runtime 启动日志。
- 测试结束恢复原 `otadata`，再次启动确认原固件 `xiaozhi 2.4.0` 从 `ota_0` 运行；SQLite 数据仍可加载 7 个事件、8 个提醒、0 条笔记。
- 本次只证明分区兼容、写入校验、启动和恢复；没有把启动日志当成 WSS、ASR、TTS 或 I2S 音频闭环证据。
- 本次启动 smoke 未采集最低空闲堆统计；该项随真实 Transport/音频链路接入补测。

ESP-IDF 6.0.2 自带的 `otatool.py write_ota_partition` 在本次写入阶段触发参数错误：`_write_ota_partition() got an unexpected keyword argument 'input'`。在确认目标地址、镜像大小和备份完整后，使用 esptool 115200、显式地址 `0x410000` 写入并回读校验作为回退路径：

```bash
python -m esptool --chip esp32s3 -p /dev/cu.usbmodemXXXX -b 115200 \
  write-flash 0x410000 build/esp32s3-dev/voicelife.bin
python -m esptool --chip esp32s3 -p /dev/cu.usbmodemXXXX -b 115200 \
  read-flash 0x410000 0x2a1c0 /tmp/voicelife-ota1-readback.bin
shasum -a 256 /tmp/voicelife-ota1-readback.bin
```

将回读文件与构建产物逐字节比对后再切换 `otadata`；不要因此改用 460800/921600，也不要把回退命令扩展成全量刷写。

## 6. PR 最低证据

硬件 PR 至少写清板卡、固件 SHA-256、App 大小、目标槽、串口波特率、测试步骤、结果、最低空闲堆和恢复结果。失败同样是有效证据；真正不能接受的是没有恢复路径的“再试一次”。
