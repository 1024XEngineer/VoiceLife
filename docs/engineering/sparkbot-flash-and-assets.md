# ESP-SparkBot 固件刷写与 assets 资源清单

本清单定义 ESP-SparkBot 固件的刷写范围与 assets 分区资源校验步骤。
**在确认实板分区表与授权范围前，不得刷写 bootloader、分区表、factory、
assets 或任何未知数据分区。**

## 1. 分区表（官方实板布局）

`partitions_sparkbot.csv`（与实板验证的小智官方 2.4.0 一致）：

| Name | Type | SubType | Offset | Size |
| --- | --- | --- | --- | --- |
| nvs | data | nvs | 0x009000 | 0x004000 |
| otadata | data | ota | 0x00d000 | 0x002000 |
| phy_init | data | phy | 0x00f000 | 0x001000 |
| factory | app | factory | 0x010000 | 0x2F0000 |
| assets | data | spiffs | 0x300000 | 0x100000 |

## 2. assets 分区镜像（构建期生成）

- 生成脚本：`scripts/build_sparkbot_assets.py`（官方 12B 头 + 44B/项文件表 +
  "ZZ" magic + checksum 格式，回读校验后输出 SHA-256）。
- 输入：`components/voicelife_display_esp/assets/esp-sparkbot/mascot/gifs/`
  的 10 个官方牛头 GIF（96x96，总 142683 字节）。
- 构建产物：`${build_dir}/sparkbot_assets.bin`（根 CMakeLists 钩子
  `sparkbot_assets` target 生成，GIF/脚本变更触发重建）。
- 资源包 SHA-256 记录于 `manifest.json` 的 `budget.assets_image_sha256`。

## 3. 刷写范围（实板阶段，需再次确认分区表）

授权范围仅：应用固件（factory）与 assets 分区。示例（esptool）：

```bash
# 应用固件（SparkBot Profile 构建产物，Kconfig 须含
# CONFIG_VOICELIFE_BOARD_ESP_SPARKBOT=y，构建后检查生成的 sdkconfig）
esptool.py --port /dev/cu.usbmodem14101 write_flash 0x10000 build/voicelife.bin
# assets 分区镜像
esptool.py --port /dev/cu.usbmodem14101 write_flash 0x300000 build/sparkbot_assets.bin
```

禁止覆盖：bootloader、分区表、nvs、otadata、phy_init、factory（除上述应用
固件写入）、未知数据分区；不输出 NVS 内容。

## 4. 烧录后校验

1. 启动日志依次出现 `SparkBotAssembly` 实例化、ST7789/LVGL 初始化、
   显示任务启动、assets 分区 mmap 成功。
2. assets 分区校验和匹配（`SparkBotEmojiAssets::Initialize` 返回 Ok）。
3. 10 个受控 asset_id（boot/connecting/error/happy/idle/listening/
   provisioning/sleepy/speaking/thinking）全部可 `Load`，`ZZ` magic 校验通过。
4. 状态映射（EmotionKeyForMood）与 GIF 播放节奏符合官方。
