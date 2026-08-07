# Source 17：跨 MCU 音频与存储候选矩阵

- 来源：各厂商板卡资料、Zephyr 官方板卡/VFS 文档、SQLite VFS 与损坏机制文档
- 读取日期：2026-08-04
- 类型：厂商和上游项目一手资料

## 决定

ESP32-S3 仍是唯一真实支持平台。通用化的对象是 Profile、探针协议、结果格式与恢复守卫，不是一份固件覆盖所有 MCU。首个跨平台验证批次建议为 Arduino GIGA R1 WiFi、Raspberry Pi Pico 2、NXP MIMXRT1060-EVKB；它们分别覆盖 STM32H747、RP2350 Arm/RISC-V、i.MX RT1062，以及 QSPI NOR、共享 XIP Flash、microSD 三类存储路径。

| 候选板 | 价值 | 进入实现前的阻断项 |
| --- | --- | --- |
| Arduino GIGA R1 WiFi | STM32H747、16 MB QSPI、8 MB SDRAM，具备直接关电引脚 | I2S/Codec、Zephyr/mbed 后端、真断电恢复 |
| Raspberry Pi Pico 2 | 520 KB SRAM、4 MB Flash，同板覆盖 Arm 与 RISC-V | 无板载网络、固件与数据 Flash 分区、音频输入 |
| MIMXRT1060-EVKB | 1 MB 片上 RAM、32 MB SDRAM、QSPI 和 microSD | 网络路径、音频 Codec、外部断电控制 |
| STM32H7B3I-DK | 资源充足，OSPI NOR、SDRAM、microSD 完整 | 成本和板卡复杂度较高，列为第二批 |
| nRF5340 DK | 低功耗边界，8 MB QSPI | 1 MB 代码 Flash 对 SQLite/Opus 偏紧，外部网络未定 |
| Teensy 4.1 | i.MX RT1062、SD/QSPI 灵活 | Zephyr 支持状态弱，需要独立后端 |

统一工具边界为：

```text
BoardProfile
  -> BuildAdapter
  -> FlashAdapter
  -> FaultInjector(reset | power_cut | brownout)
  -> ProbeProtocol
  -> ResultVerifier
  -> RestoreGuard
```

新增板卡必须依次通过 capability probe、host contract、board smoke、resource budget、recovery。芯片有 I2S 或文件系统能挂载都不等于语音/SQLite 可用；Codec、功放、VFS `sync`、磨损均衡和真实掉电必须单独实测。

主要资料：

- <https://docs.zephyrproject.org/latest/services/storage/file_system/index.html>
- <https://www.sqlite.org/vfs.html>
- <https://www.sqlite.org/howtocorrupt.html>
- <https://docs.arduino.cc/hardware/giga-r1-wifi/>
- <https://docs.zephyrproject.org/latest/boards/raspberrypi/rpi_pico2/doc/index.html>
- <https://docs.zephyrproject.org/latest/boards/nxp/mimxrt1060_evk/doc/index.html>
