# VoiceLife 本周 PR 代码溯源（汇报版）

一句话结论：这份压缩版只保留“看哪些文件、主链路怎么走、为什么要这样整理”，方便你汇报时快速讲清楚，不展开实现细节。

一句话动作：先看总链路，再看 9 个 PR 的文件位置；细节留给 [完整版本](./voicelife-code-trace-20260807.md)。

## 为什么要压缩

这份版面是给汇报用的，不是给自己啃代码用的。

- 汇报时，听众通常只需要知道“改到了哪些文件”和“数据/调用怎么流动”。
- 具体函数内部怎么写，现场说太细只会打断主线。
- 先把文件位置和主链路钉住，后面追问时再回到完整文档和代码。

## 总链路

```text
main/main.cc
  -> runtime::Start()
  -> SpeechProviderRegistry
  -> VoiceSession
  -> Provider
  -> Transport
  -> 云端

I2S 麦克风
  -> AudioInputPort
  -> VoiceSession
  -> Provider
  -> Transport
  -> 云端

云端
  -> Transport
  -> Provider
  -> VoiceSession
  -> AudioOutputPort
  -> I2S 扬声器

业务模块
  -> Schedule/Timing Store Port
  -> StorageTransactionPort
  -> SQLite
  -> FATFS / Wear Levelling
  -> Flash
```

## 九个 PR

### #92 架构主干

文件位置：

- [`main/main.cc`](/Users/mac/Desktop/project/VoiceLife-latest/main/main.cc)
- [`runtime.cc`](/Users/mac/Desktop/project/VoiceLife-latest/components/voicelife_runtime/src/runtime.cc)
- [`voice_ports.h`](/Users/mac/Desktop/project/VoiceLife-latest/components/voicelife_voice/include/voicelife/voice/voice_ports.h)
- [`voice_session.cc`](/Users/mac/Desktop/project/VoiceLife-latest/components/voicelife_voice/src/voice_session.cc)
- `components/voicelife_voice/src/voice_provider_registry.cc`

主链路：

```text
app_main -> runtime::Start() -> Registry -> Provider -> VoiceSession
```

### #104 数据库协议和实板验证

文件位置：

- [`storage_protocol.h`](/Users/mac/Desktop/project/VoiceLife-latest/components/voicelife_storage_sqlite/include/voicelife/storage_sqlite/storage_protocol.h)
- [`storage_protocol.cc`](/Users/mac/Desktop/project/VoiceLife-latest/components/voicelife_storage_sqlite/src/storage_protocol.cc)
- [`storage_protocol_contract_test.cc`](/Users/mac/Desktop/project/VoiceLife-latest/tests/host/storage_protocol_contract_test.cc)
- [`sqlite_board_probe.py`](/Users/mac/Desktop/project/VoiceLife-latest/scripts/sqlite_board_probe.py)
- [`board-storage-validation.md`](/Users/mac/Desktop/project/VoiceLife-latest/docs/engineering/board-storage-validation.md)

主链路：

```text
业务模块 -> Store Port -> StorageTransactionPort -> SQLite -> FATFS/WL -> Flash
```

### #106 可插拔语音会话

文件位置：

- [`voice_session.cc`](/Users/mac/Desktop/project/VoiceLife-latest/components/voicelife_voice/src/voice_session.cc)
- [`voice_provider_registry.cc`](/Users/mac/Desktop/project/VoiceLife-latest/components/voicelife_voice/src/voice_provider_registry.cc)
- [`linx_speech_provider.h`](/Users/mac/Desktop/project/VoiceLife-latest/components/voicelife_linx/include/voicelife/linx/linx_speech_provider.h)
- [`linx_speech_provider.cc`](/Users/mac/Desktop/project/VoiceLife-latest/components/voicelife_linx/src/linx_speech_provider.cc)
- [`linx_json_codec.cc`](/Users/mac/Desktop/project/VoiceLife-latest/components/voicelife_linx/src/linx_json_codec.cc)

主链路：

```text
Runtime -> Registry -> Provider -> VoiceSession -> hello / listen / abort
```

### #108 Linx WSS 传输

文件位置：

- [`esp_websocket_transport.h`](/Users/mac/Desktop/project/VoiceLife-latest/components/voicelife_linx_esp/include/voicelife/linx_esp/esp_websocket_transport.h)
- [`esp_websocket_transport.cc`](/Users/mac/Desktop/project/VoiceLife-latest/components/voicelife_linx_esp/src/esp_websocket_transport.cc)
- [`esp_websocket_impl.cc`](/Users/mac/Desktop/project/VoiceLife-latest/components/voicelife_linx_esp/src/esp_websocket_impl.cc)
- [`esp_websocket_events.cc`](/Users/mac/Desktop/project/VoiceLife-latest/components/voicelife_linx_esp/src/esp_websocket_events.cc)

主链路：

```text
VoiceSession -> Linx Provider -> EspWebSocketTransport -> WSS 云端
```

### #110 板级 Profile 和探针

文件位置：

- [`audio_board_profile.h`](/Users/mac/Desktop/project/VoiceLife-latest/components/voicelife_audio_esp/include/voicelife/audio_esp/audio_board_profile.h)
- [`audio_board_profile.cc`](/Users/mac/Desktop/project/VoiceLife-latest/components/voicelife_audio_esp/src/audio_board_profile.cc)
- [`esp32s3_audio_probe.h`](/Users/mac/Desktop/project/VoiceLife-latest/components/voicelife_audio_esp/include/voicelife/audio_esp/esp32s3_audio_probe.h)
- [`esp32s3_audio_probe.cc`](/Users/mac/Desktop/project/VoiceLife-latest/components/voicelife_audio_esp/src/esp32s3_audio_probe.cc)

主链路：

```text
Runtime -> AudioBoardProfile -> Esp32s3AudioProbe -> I2S / GPIO / DMA
```

### #112 纯 I2S PCM Profile

文件位置：

- [`audio_board_profile.h`](/Users/mac/Desktop/project/VoiceLife-latest/components/voicelife_audio_esp/include/voicelife/audio_esp/audio_board_profile.h)
- [`audio_board_profile.cc`](/Users/mac/Desktop/project/VoiceLife-latest/components/voicelife_audio_esp/src/audio_board_profile.cc)
- `config/profiles/esp32s3-voicelife-pcb-pcm.json`

主链路：

```text
Profile -> I2S RX/TX -> PCM 对齐 -> Audio Probe / Audio Port
```

### #114 PCM Audio Port

文件位置：

- [`esp32s3_pcm_audio_port.h`](/Users/mac/Desktop/project/VoiceLife-latest/components/voicelife_audio_esp/include/voicelife/audio_esp/esp32s3_pcm_audio_port.h)
- [`esp32s3_pcm_audio_port.cc`](/Users/mac/Desktop/project/VoiceLife-latest/components/voicelife_audio_esp/src/esp32s3_pcm_audio_port.cc)
- [`esp32s3_pcm_audio_port_internal.h`](/Users/mac/Desktop/project/VoiceLife-latest/components/voicelife_audio_esp/src/esp32s3_pcm_audio_port_internal.h)
- [`esp32s3_pcm_i2s_runtime.cc`](/Users/mac/Desktop/project/VoiceLife-latest/components/voicelife_audio_esp/src/esp32s3_pcm_i2s_runtime.cc)
- `components/voicelife_voice/src/pcm_frame_assembler.cc`

主链路：

```text
I2S 麦克风 -> CaptureTask -> PCM Period -> PcmFrameAssembler -> AudioInputPort -> VoiceSession
VoiceSession -> AudioOutputPort -> OutputTask -> I2S 扬声器
```

### #117 README 清理

文件位置：

- [`README.md`](/Users/mac/Desktop/project/VoiceLife-latest/README.md)

主链路：

```text
删除过时说明 -> 保留当前使用说明
```

### #151 文档边界

文件位置：

- `docs/engineering/document-placement.md`

主链路：

```text
研究资料 -> Issue / PR Review -> 稳定文档入库
```

## 为什么这样讲

- 先给文件位置，别人能立刻打开看。
- 再给主链路，别人知道系统怎么串。
- 不讲实现细节，是为了汇报时不跑题。
- 真要深挖，再回到完整文档和源代码。
