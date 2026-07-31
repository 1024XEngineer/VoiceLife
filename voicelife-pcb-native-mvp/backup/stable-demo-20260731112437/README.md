# VoiceLife PCB 固件备份：稳定演示版本

- 备份时间：2026-07-31 11:24:37 CST
- 板卡串口：`/dev/cu.usbmodem5A840116301`
- 芯片：ESP32-S3
- 固件来源：`firmware/build-idf6`
- 数据分区：`voicelife`，offset `0xE00000`，size `0x200000`

## 备份文件

| 文件 | SHA-256 | 说明 |
| --- | --- | --- |
| `xiaozhi.bin` | `e586e2a76de7a51379f20e325a1d865a5ce7a5ec4f85dc0bcec1a16b5859b656` | 稳定演示版本应用固件 |
| `bootloader.bin` | `378e69a56a189134e09065ed3bfd558cb19a5d400513adbaa7671d5b4d40863e` | bootloader |
| `partition-table.bin` | `53dfebb2737bedfa32368fa5f2f6f5d75f1874d0d938d0a5a94f43b5d0aa6bc8` | 构建产物分区表 |
| `stable-demo-partition-table-live-20260731112437.bin` | `349594f3de690464855839e78e9044dae5a3ea808aca01373a047758dac61f6c` | 板上实时读取分区表 |
| `stable-demo-voicelife-full-before-clear-20260731112437.bin` | `ded482aa2127e1c16e5c08cb71ac4e3f9e072cb1898060bb51541979f22a1e68` | 清理前完整 `voicelife` 数据分区备份 |
| `stable-demo-voicelife-first64k-before-clear-20260731112437.bin` | `789719ba49d81f70d66ed4e8d84a48bb76129409b1998d57563a236157a166e6` | 清理前 `voicelife` 前 64KB 样本 |
| `voicelife-first64k-after-final-clear.bin` | `71189f7fb6aed638640078fba3a35fda6c39c8962e74dcc75935aac948da9063` | 最终清理后 `voicelife` 前 64KB 样本，已校验全 `0xFF` |

## 状态

- 这份目录保留修改唤醒词前的稳定演示固件与板上数据备份。
- 板载历史日程数据已在烧录新固件后擦除，最终样本校验结果为全 `0xFF`。
