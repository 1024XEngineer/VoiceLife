# 第三方来源

本仓库没有直接复制小智固件业务代码。以下工具基于其公开实现思路做了收敛和改写，继续遵循上游 MIT 许可：

| 本仓库文件 | 上游来源 | 迁移方式 |
| --- | --- | --- |
| `scripts/firmware.py` | [`78/xiaozhi-esp32/scripts/build.py`](https://github.com/78/xiaozhi-esp32/blob/dd99da00dc4c89ed4ab07fcec038c03f13f4de50/scripts/build.py) | 保留 Profile 驱动构建、合并与打包思路；移除多板发布矩阵、语言和资源耦合 |
| `scripts/audio_debug_server.py` | [`78/xiaozhi-esp32/scripts/audio_debug_server.py`](https://github.com/78/xiaozhi-esp32/blob/dd99da00dc4c89ed4ab07fcec038c03f13f4de50/scripts/audio_debug_server.py) | 保留 UDP PCM 抓取能力；补齐地址、端口、音频格式和输出参数 |

上游仓库：[`78/xiaozhi-esp32`](https://github.com/78/xiaozhi-esp32)，迁移调研基线 `dd99da00dc4c89ed4ab07fcec038c03f13f4de50`。MIT 许可原文见 [`third_party/licenses/xiaozhi-esp32-MIT.txt`](./third_party/licenses/xiaozhi-esp32-MIT.txt)。
