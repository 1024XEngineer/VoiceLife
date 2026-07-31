# PCB VoiceLife MVP 固件交付记录

本文记录 Issue #88 对应的 PCB 原生固件当前交付状态：已备份稳定演示版本，已将当前测试板唤醒词切换为“你好牛牛”，并已清空板载历史日程数据用于干净环境测试。

## 交付范围

- 关联 Issue：[PCB VoiceLife MVP #88](https://github.com/1024XEngineer/XE6-15/issues/88)
- 固件工作区：`voicelife-pcb-native-mvp/firmware`
- 固件分支：`voice-life-native-mvp`
- 固件提交：
  - `e3ecb34 feat(voicelife): add native PCB MVP firmware`
  - `0de64f7 fix(voicelife): switch PCB wake word to niu niu`
- 当前板卡：ESP32-S3，`/dev/cu.usbmodem5A840116301`

## 稳定演示版本备份

- 备份目录：`voicelife-pcb-native-mvp/backup/stable-demo-20260731112437`
- 标注：稳定演示版本
- 应用固件：`xiaozhi.bin`
- 应用固件 SHA-256：`e586e2a76de7a51379f20e325a1d865a5ce7a5ec4f85dc0bcec1a16b5859b656`
- 清理前完整 `voicelife` 数据分区已备份。

## 唤醒词变更

- 原唤醒词：`你好小智`
- 新唤醒词：`你好牛牛`
- 当前 PCB 构建启用自定义唤醒词：`ni hao niu niu`
- 当前 PCB 构建未启用旧 `NIHAOXIAOZHI` WakeNet 模型。

## 验证记录

- Host 测试：`scripts/run_voicelife_host_tests.sh`，通过。
- 固件构建：`python3 scripts/build.py bread-compact-wifi --name voicelife-pcb`，通过。
- 烧录：已写入并校验 ESP32-S3。
- 数据清理：已擦除 `voicelife` 分区 `0xE00000..0xFFFFFF`；最终前 64KB 回读样本全为 `0xFF`。
- 清理后样本 SHA-256：`71189f7fb6aed638640078fba3a35fda6c39c8962e74dcc75935aac948da9063`
