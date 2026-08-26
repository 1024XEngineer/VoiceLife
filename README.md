<div align="center">

<h1 align="center">
  <img src="./assets/logo.svg" alt="VoiceLife 声活 Logo" width="350"  valign="middle" />
</h1>

<p><strong>面向本地日程、提醒、存储、语音和消息渠道的设备端系统</strong></p>

<p>
几个模块一起交付：日程负责记录安排，定时任务负责触发，设备运行时负责组装，语音、存储和 IM 通过适配器接入。
</p>

<p>
<a href="#快速开始">快速开始</a> ·
<a href="#模块边界">模块边界</a> ·
<a href="#开发方式">开发方式</a> ·
<a href="./CONTRIBUTING.md">参与开发</a>
</p>

<p>
<a href="https://github.com/1024XEngineer/VoiceLife/actions/workflows/ci.yml"><img src="https://img.shields.io/github/actions/workflow/status/1024XEngineer/VoiceLife/ci.yml?branch=main&style=flat-square&label=CI" alt="CI" /></a>
<a href="https://codecov.io/gh/1024XEngineer/VoiceLife"><img src="https://codecov.io/gh/1024XEngineer/VoiceLife/branch/main/graph/badge.svg" alt="Codecov coverage" /></a>
<img src="https://img.shields.io/badge/ESP--IDF-6.0.2-E7352C?style=flat-square" alt="ESP-IDF 6.0.2" />
<img src="https://img.shields.io/badge/Target-ESP32--S3-222222?style=flat-square" alt="ESP32-S3" />
<img src="https://img.shields.io/badge/C%2B%2B-20-00599C?style=flat-square" alt="C++ 20" />
</p>

</div>

<p align="center">
  <img src="./docs/assets/concept.png" alt="VoiceLife 声活产品概念图" width="47%" />
  <img src="./docs/assets/牛牛_三视图总图_厚牛头.png" alt="牛牛设备三视图" width="47%" />
</p>

> [!IMPORTANT]
> 当前仓库是可编译、可测试的设备端架构主干，不是已经完成所有外部接入的成品。日程和定时任务可以用内存适配器串联；设备存储基础链路已经接入 Runtime，实板恢复验证、语音服务和 IM 平台仍按各自 Issue 验收。

## 项目在解决什么问题

VoiceLife 把安排、触发和通知拆开，让它们可以分别演进：

- 日程记录“要做什么”。
- 定时任务记录“什么时候触发哪一次”。
- 存储模块负责原子写入、重启恢复和健康指标。
- 语音与 IM 模块把外部平台转换成稳定的领域事件。
- Runtime 只负责把选定的实现组装起来，不承载业务规则。

这套拆分是为了让不同模块可以并行开发，也让某个外部平台暂时不可用时不影响其他模块的主机测试。

## 快速开始

需要 CMake。构建设备固件时还需要 ESP-IDF 6.0.2；Ninja 可选。

```bash
# 提交前完整检查，不需要 ESP-IDF
./scripts/run_pre_submit_checks.sh

# 运行指定的主机测试
./scripts/run_host_tests.sh -R schedule_policy_test

# 查看和校验固件 Profile
python3 scripts/firmware.py list
python3 scripts/firmware.py validate
```

IM Gateway 的 PostgreSQL 契约测试使用仓库根目录的 Compose 配置：

```bash
docker compose up -d postgres
pnpm --dir services/im-gateway test
```

没有 PostgreSQL 时，相关契约测试会跳过，其余测试仍可运行。

构建设备固件：

```bash
source /path/to/esp-idf-v6.0.2/export.sh
python3 scripts/prepare_sqlite.py
python3 scripts/firmware.py build esp32s3-storage-dev
python3 scripts/firmware.py package esp32s3-storage-dev
```

## 模块边界

VoiceLife 使用 ESP-IDF 组件化模块单体。核心代码使用 C++，外部 SDK、网络库、平台格式和板卡驱动只能通过 Port 或 Adapter 进入。

| 组件 | 负责什么 | 主要依赖 |
| --- | --- | --- |
| `voicelife_contracts` | Status、Result、事件和跨模块公共契约 | 无 |
| `voicelife_schedule` | 日程实体、命令、结果和服务接口 | contracts |
| `voicelife_timing` | 定时任务、实例和提醒规则 | contracts |
| `voicelife_storage_fatfs` | Flash 分区校验、Wear Levelling、FATFS 挂载生命周期和容量 | contracts；ESP 端依赖 fatfs、esp_partition |
| `voicelife_storage_sqlite` | SQLite 连接、Schema/迁移、完整性检查、业务 SQL 与 Repository | contracts、schedule；设备 Profile 启用时需要 sqlite3、FATFS/WL |
| `voicelife_im` | 平台无关的 IM 事件、上报和传输契约 | contracts |
| `voicelife_voice` | 语音会话、音频/传输 Port 和 Provider Registry | contracts |
| `voicelife_linx` | Linx/XRobot 协议和 Provider Adapter | contracts、voice |
| `voicelife_linx_esp` | ESP32-S3 WSS/TLS Transport 和分片重组 | contracts、linx |
| `voicelife_audio_esp` | ESP32-S3 音频 Profile、探针和设备端 Port | contracts、voice |
| `voicelife_board_esp` | ESP-SparkBot 板级 Profile、能力矩阵、共享电源仲裁和身份探针 | contracts |
| `voicelife_mcp` | 工具 Schema、注册中心和调用路由 | contracts |
| `voicelife_runtime` | 唯一组装入口，按生命周期启动和回滚基础设施 | contracts、mcp、voice、linx、storage adapters |

依赖方向只有一条：适配器依赖用例，用例依赖领域，领域不认识 ESP-IDF、HTTP 或平台 SDK。CI 会运行 `scripts/check_architecture.sh` 检查组件清单、命名空间和依赖图。

## 开发方式

每个模块先写稳定契约，再接入真实实现。主机测试覆盖状态、错误和跨模块串联；设备测试只验证设备才能证明的内容，例如分区恢复、I2S 生命周期和资源水位。

Profile 描述一次固件选择哪些实现，不保存凭据：

```json
{
  "id": "esp32s3-dev",
  "target": "esp32s3",
  "adapters": {
    "audio": { "driver": "scaffold", "capabilities": [] },
    "speech": { "driver": "scaffold", "capabilities": [] },
    "storage": { "driver": "memory", "capabilities": ["atomic-calendar-write"] },
    "im": { "driver": "disabled", "capabilities": [] }
  }
}
```

凭据只使用 `secret://`、`nvs://` 或 `env://` 引用，不写进 Profile、日志或 Git。新增平台时，实现 Adapter、声明能力、补契约测试，再修改部署配置；业务模块不写平台判断。

## 当前状态

| 方向 | 状态 | 说明 |
| --- | --- | --- |
| 组件边界和依赖检查 | 已完成 | 主机与 CI 可验证 |
| 日程模块 | 主机契约已覆盖 | 主机集成测试已跑通 SQLite 创建与查询最小链路；设备 Runtime 暂不装配日程业务，修改、取消和撤销仍待接入 |
| 定时任务模块 | 主机契约已覆盖 | 已支持创建、更新、取消和提醒规则，注册与唯一性语义已固定 |
| SQLite 存储资格测试 | 已完成基线 | FATFS/WL 路线通过，断电和长期磨损仍待补测 |
| IM Gateway | 开发中 | 已接入 PostgreSQL 持久化与重启恢复，平台渠道仍在补齐 |
| 语音、音频和 Linx 适配器 | 开发中 | WSS Transport 可构建并有主机契约，真实云端与声学闭环未完成 |
| Profile 驱动 Runtime | 基础存储已接入 | storage profile 可组装 FATFS/WL、SQLite、Schema 检查；其他适配器切换继续补齐 |
| 真机闭环与用户试用 | 未开始 | 进入对应功能 Issue 后再验收 |

## 仓库结构

```text
VoiceLife/
├── components/       # C++ 组件
├── config/            # Profile 和 Schema，不放凭据
├── docs/              # 架构、工程和协作文档
├── main/              # ESP-IDF app_main
├── scripts/           # 构建、测试、检查和设备恢复工具
├── services/          # 设备外的服务，例如 IM Gateway
├── tests/              # 主机、Python 和必须上板的测试
└── third_party/       # 第三方源码和许可证
```

README 只保留项目入口和跨模块信息。当前维护文档按[文档导航](./docs/README.md)组织；研究、阶段草稿和一次性证据留在对应 Issue、PR 或 Git 历史，避免把某一个模块的过程材料当成整个项目的产品说明。

## 相关入口

- [架构与适配器设计规范](./docs/architecture/design-guidelines.md)
- [ADR 0001：组件化模块单体与 Ports/Adapters](./docs/adr/0001-component-modular-hexagonal.md)
- [ADR 0002：能力驱动的适配器 Profile](./docs/adr/0002-capability-driven-adapters.md)
- [SQLite 实板验证与 Flash 恢复手册](./docs/engineering/board-storage-validation.md)
- [硬件调试与串口日志规则](./docs/engineering/hardware-debugging.md)
- [SparkBot 显示组件说明](./docs/components/sparkbot-display.md)
- [IM Gateway 运行手册](./docs/services/im-gateway.md)
- [协同开发规范](./docs/engineering/collaboration.md)
- [提交描述规范](./docs/engineering/commit-convention.md)
- [参与开发](./CONTRIBUTING.md)

## 致谢

设备侧部分实现参考了 [`78/xiaozhi-esp32`](https://github.com/78/xiaozhi-esp32) 的音频、协议和构建经验。具体迁移范围和许可证记录在 [THIRD_PARTY.md](./THIRD_PARTY.md)。
