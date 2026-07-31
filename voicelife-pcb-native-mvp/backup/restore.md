# 原始固件恢复

## 备份记录

- 文件：`esp32s3-original-flash-20260729.bin`
- 地址：`0x00000000`
- 长度：`0x01000000`（16,777,216 bytes）
- 芯片：ESP32-S3 QFN56，revision v0.2
- Flash：16 MB，3.3 V，Quad
- PSRAM：8 MB
- MAC：`98:a3:16:e6:91:dc`
- 串口：`/dev/cu.usbmodem5A840116301`
- SHA-256：`4e3ea1bd77873dc2b300f7b14adf0c3b5b93ceb15a8febe15d1c19464b76385d`

## 写回命令

以下命令会覆盖整片 Flash，只能在确认目标端口和镜像后执行：

```bash
/Users/mac/Library/Python/3.9/bin/esptool.py \
  --chip esp32s3 \
  --port /dev/cu.usbmodem5A840116301 \
  --baud 460800 \
  --before default_reset \
  --after hard_reset \
  write_flash --flash_size keep \
  0x00000000 backup/esp32s3-original-flash-20260729.bin
```

写回后验证：

```bash
/Users/mac/Library/Python/3.9/bin/esptool.py \
  --chip esp32s3 \
  --port /dev/cu.usbmodem5A840116301 \
  --baud 460800 \
  --before default_reset \
  --after hard_reset \
  verify_flash --flash_size keep \
  0x00000000 backup/esp32s3-original-flash-20260729.bin
```

如果自动复位不能进入下载模式，按住 BOOT，点按一次 RST，松开 BOOT 后重试。不要先执行 `erase_flash`。
