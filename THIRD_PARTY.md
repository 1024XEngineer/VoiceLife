# 第三方来源

## yyjson

`third_party/yyjson/yyjson.c` 与 `third_party/yyjson/yyjson.h` 原样来自
[`ibireme/yyjson`](https://github.com/ibireme/yyjson) `0.12.0`，固定上游 commit
`8b4a38dc994a110abaec8a400615567bd996105f`。VoiceLife 使用其严格 RFC 8259
读取模式构建临时 DOM，并使用可变文档生成 MCP JSON；第三方类型不进入公共 API。

| 文件 | SHA-256 |
| --- | --- |
| `third_party/yyjson/yyjson.c` | `ac2e9bbb2e2d9149d90878d40506a1d624fa0b33c979a11b61075c54782c6d6a` |
| `third_party/yyjson/yyjson.h` | `175867c5493a5df648cec566717fa1c29aa2f6096f5f0cf1efad0b65e1f6d7b3` |
| `third_party/licenses/yyjson-MIT.txt` | `45e384d3d52c73cba3a64d6e6c25d47cd738cd8a55c30629e3201046eda62947` |

MIT 许可原文见 [`third_party/licenses/yyjson-MIT.txt`](./third_party/licenses/yyjson-MIT.txt)。

## 小智迁移参考

本仓库没有直接复制小智固件业务代码。以下工具基于其公开实现思路做了收敛和改写，继续遵循上游 MIT 许可：

| 本仓库文件 | 上游来源 | 迁移方式 |
| --- | --- | --- |
| `scripts/firmware.py` | [`78/xiaozhi-esp32/scripts/build.py`](https://github.com/78/xiaozhi-esp32/blob/dd99da00dc4c89ed4ab07fcec038c03f13f4de50/scripts/build.py) | 保留 Profile 驱动构建、合并与打包思路；移除多板发布矩阵、语言和资源耦合 |
| `scripts/audio_debug_server.py` | [`78/xiaozhi-esp32/scripts/audio_debug_server.py`](https://github.com/78/xiaozhi-esp32/blob/dd99da00dc4c89ed4ab07fcec038c03f13f4de50/scripts/audio_debug_server.py) | 保留 UDP PCM 抓取能力；补齐地址、端口、音频格式和输出参数 |

上游仓库：[`78/xiaozhi-esp32`](https://github.com/78/xiaozhi-esp32)，迁移调研基线 `dd99da00dc4c89ed4ab07fcec038c03f13f4de50`。MIT 许可原文见 [`third_party/licenses/xiaozhi-esp32-MIT.txt`](./third_party/licenses/xiaozhi-esp32-MIT.txt)。
