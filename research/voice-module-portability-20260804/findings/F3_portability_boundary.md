# F3：可迁移的是契约，不是一份万能固件

## 判断

H3 得到支持。Zephyr VFS、SQLite VFS 和各厂商 I2S 都表明“接口相似”不代表锁、sync、DMA、XIP、掉电和实时性相同。跨板卡工具应统一场景协议、AudioFrame、BoardProfile、CapabilityMatrix、结果/证据包；刷写、串口、电源和存储 VFS 留在平台 Adapter。

## 首批顺序

ESP32-S3 继续作为基线；随后用 Zephyr + 一块低资源板验证 Port，再引入带 QSPI/SD 对照的 i.MX RT 或 STM32。其他板卡先做能力探针，不增加空壳 Adapter 数量。

## 迁移闸门

新板只有在编译、启动、音频格式、资源预算、故障恢复和证据包都通过后，才能从“调研”变成“支持”。
