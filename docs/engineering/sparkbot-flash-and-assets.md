# ESP-SparkBot 固件刷写与 assets 资源清单

本清单定义 ESP-SparkBot 固件、显示资源与 MultiNet 语音模型的刷写范围。
**分区迁移前不得写入任何区域；迁移必须有明确人工授权，且不得擦除或读取 NVS
中的敏感内容。**

## 1. 分区表（官方实板布局）

`partitions_sparkbot.csv`（VoiceLife SparkBot Profile）：

| Name | Type | SubType | Offset | Size |
| --- | --- | --- | --- | --- |
| nvs | data | nvs | 0x009000 | 0x004000 |
| otadata | data | ota | 0x00d000 | 0x002000 |
| phy_init | data | phy | 0x00f000 | 0x001000 |
| factory | app | factory | 0x010000 | 0x2D0000 |
| linx_secrets | data | nvs | 0x2E0000 | 0x010000 |
| assets | data | spiffs | 0x300000 | 0x100000 |
| model | data | spiffs | 0x400000 | 0x300000 |

`model` 由 ESP-SR 官方 `srmodels_bin` target 生成，其中包含 SparkBot Profile
启用的 Chinese MultiNet7 模型。它不是 GIF assets，也不接受运行时 URL、路径或
调用方字节流。

## 2. assets 分区镜像（构建期生成）

- 生成脚本：`scripts/build_sparkbot_assets.py`（官方 12B 头 + 44B/项文件表 +
  "ZZ" magic + checksum 格式，回读校验后输出 SHA-256）。
- 输入：`components/voicelife_display_esp/assets/esp-sparkbot/mascot/gifs/`
  的 10 个官方牛头 GIF（96x96，总 142683 字节）和官方 common 16px 字体。
- 构建产物：`${build_dir}/sparkbot_assets.bin`（根 CMakeLists 钩子
  `sparkbot_assets` target 生成，GIF/脚本变更触发重建）。
- 资源包 SHA-256 记录于 `manifest.json` 的 `budget.assets_image_sha256`。

## 3. 首次迁移和后续刷写

当前实板旧分区表不含 `linx_secrets` 与 `model`，不能把新应用直接写入旧表。
首次迁移必须先获人工书面授权，并在写入前读取 `0x8000..0x8fff` 后核对旧表；只允许
写入以下四个明确目标：新分区表 `0x8000`、应用 `0x10000`、显示 assets `0x300000`、
MultiNet 模型 `0x400000`。不得写 bootloader、NVS、otadata、phy_init 或未知区域。

```bash
# 仅在明确授权的首次迁移中执行，实际端口以已连接设备为准。
esptool.py --port /dev/cu.usbmodem14101 write_flash \
  0x8000 build/esp32s3-esp-sparkbot/partition_table/partition-table.bin \
  0x10000 build/esp32s3-esp-sparkbot/voicelife.bin \
  0x300000 build/esp32s3-esp-sparkbot/sparkbot_assets.bin \
  0x400000 build/esp32s3-esp-sparkbot/srmodels/srmodels.bin
```

后续已迁移且分区表哈希、偏移和大小均一致的设备，授权范围可仅为应用、assets、model：

```bash
esptool.py --port /dev/cu.usbmodem14101 write_flash \
  0x10000 build/esp32s3-esp-sparkbot/voicelife.bin \
  0x300000 build/esp32s3-esp-sparkbot/sparkbot_assets.bin \
  0x400000 build/esp32s3-esp-sparkbot/srmodels/srmodels.bin
```

禁止覆盖：bootloader、NVS、otadata、phy_init、未知数据分区；不输出 NVS 内容。

## 4. 烧录后校验

1. 启动日志依次出现 `SparkBotAssembly` 实例化、ST7789/LVGL 初始化、
   显示任务启动、assets 分区 mmap 成功。
2. assets 分区校验和匹配（`SparkBotEmojiAssets::Initialize` 返回 Ok）。
3. 10 个受控 asset_id（boot/connecting/error/happy/idle/listening/
   provisioning/sleepy/speaking/thinking）全部可 `Load`，`ZZ` magic 校验通过。
4. 状态映射（EmotionKeyForMood）与 GIF 播放节奏符合官方。
5. 日志出现 `本地命令检测器已就绪` 与三句命令 `你好牛牛,牛牛,别说了`；唤醒确认
   必须出现 `LINX_SEND listen state=detect`、真实 `tts_started/tts_stopped` 和 I2S
   输出统计。仅代码或主机测试不构成唤醒、声音通过证据。
