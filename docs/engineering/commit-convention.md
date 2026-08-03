# VoiceLife 提交描述规范

所有人工提交统一使用“受控 Gitmoji + Conventional Commit 结构 + 中文描述”。目标是让提交历史能直接回答改了什么、为什么改、影响哪个边界，并能被 changelog、Review 和回退工具稳定解析。

## 1. 标准格式

```text
<gitmoji> <type>(<scope>): <中文主题>

<为什么要改，之前有什么问题>
<这次采用什么方案，有什么取舍>
<验证结果和仍然存在的风险>

Refs #<issue>
BREAKING CHANGE: <不兼容变化与迁移方式>
```

示例：

```text
🏗️ refactor(architecture): 建立组件化 Ports 和 Adapters 主干

将日程、定时、MCP、Voice 与 IM 拆为独立 ESP-IDF component，
业务核心不再依赖协议和板卡实现。

主机串联测试与 ESP32-S3 构建已通过；真实音频和持久化仍为后续范围。

Refs #123
```

## 2. 主题规则

- 必须以允许清单中的一个 Gitmoji 开头，emoji 后空一格。
- `type` 使用小写固定值；有明确模块时写 `scope`。
- 冒号后用中文概括完成的动作，技术名词可保留英文。
- 使用“建立、修复、拒绝、迁移、补充”这类具体动词，不写“优化一下”“更新代码”。
- 主题不超过 72 个字符，结尾不加句号。
- 一个主题只表达一个意图。出现“以及、同时、顺便”时，先判断是否应该拆提交。

## 3. Type 与 Scope

| Type | 用途 |
| --- | --- |
| `feat` | 新增用户或系统可观察能力 |
| `fix` | 修复错误、安全或隐私问题 |
| `docs` | 只修改文档和说明 |
| `refactor` | 不改变外部行为的结构调整和架构变化 |
| `perf` | 可测量的性能或资源占用改进 |
| `test` | 新增、修正测试或 fixture |
| `build` | 构建、依赖、配置和开发脚本 |
| `ci` | CI 工作流和自动化检查 |
| `chore` | 发布、清理等不属于以上类型的维护工作 |
| `revert` | 回退一个已有提交 |

优先使用组件名作为 Scope：`schedule`、`timing`、`application`、`mcp`、`voice`、`im`、`platform`、`runtime`。工程类可用 `architecture`、`config`、`tooling`、`ci`、`deps`、`readme`、`release`。

不要使用个人姓名、模糊的 `core`、`misc` 或 Issue 编号作为 Scope。

## 4. 允许的 Gitmoji

清单依据 [gitmoji.dev API](https://gitmoji.dev/api/gitmojis)，于 2026-08-03 核对。为了让历史稳定，项目只允许以下子集：

| Emoji | Code | 允许 Type | 场景 |
| --- | --- | --- | --- |
| ✨ | `:sparkles:` | `feat` | 新能力 |
| 🐛 | `:bug:` | `fix` | 普通缺陷 |
| 🔒️ | `:lock:` | `fix` | 安全或隐私修复 |
| 📝 | `:memo:` | `docs` | 文档 |
| ♻️ | `:recycle:` | `refactor` | 重构 |
| 🏗️ | `:building_construction:` | `refactor` | 架构变化，Scope 应为 `architecture` 或具体模块 |
| ⚡️ | `:zap:` | `perf` | 性能与资源优化 |
| ✅ | `:white_check_mark:` | `test` | 测试 |
| 👷 | `:construction_worker:` | `ci` | CI |
| 🔧 | `:wrench:` | `build` / `chore` | 配置 |
| 🔨 | `:hammer:` | `build` | 开发脚本 |
| ⬆️ | `:arrow_up:` | `build` | 依赖升级 |
| 🔥 | `:fire:` | `refactor` / `chore` | 删除代码或文件 |
| 🚚 | `:truck:` | `refactor` / `chore` | 移动或重命名 |
| 🔖 | `:bookmark:` | `chore` | 版本发布 |
| ⏪️ | `:rewind:` | `revert` | 回退 |

一个提交只用一个主 emoji。不要在正文或主题末尾堆 emoji，也不要自行扩充清单；新增 emoji 需要先更新规范和检查脚本。

## 5. 正文怎么写

小改动可以没有正文；以下情况必须写正文：

- 改变组件边界、接口、数据模型或持久化格式；
- 修复并发、掉电恢复、安全、隐私或幂等问题；
- 行为从代码 diff 里看不出原因；
- 有未解决风险、兼容窗口或迁移步骤；
- 关联 Design、Proposal、Issue 或外部上游提交。

正文优先中文，按“原因 → 方案与取舍 → 验证/风险”组织。不要复述文件列表；不要写“完善相关逻辑”“增强稳定性”而不给具体场景。

引用规则：

- `Refs #123`：有关联，但本提交不单独关闭 Issue。
- `Closes #123`：该提交随 PR 合并后足以完成 Issue 的全部验收。
- `BREAKING CHANGE:`：接口、Profile Schema、持久化或协议不兼容时必填，并写迁移办法。
- 上游迁移写 `Upstream: 78/xiaozhi-esp32@<sha>`。

## 6. 提交拆分

应该放在同一提交：

- 一个行为变化及直接对应的测试；
- 一个接口变化及所有必须同步编译的调用方；
- 一个文件移动及为保持构建通过所需的路径更新。

应该拆开：

- 机械移动与业务改写；
- 架构重构与新功能；
- 依赖升级与功能开发；
- README 素材更新与无关代码；
- 可独立回退的两个修复。

每个提交在自己的位置都应通过编译和相关测试。禁止提交构建产物、真实凭据、设备备份和本地测试证据。

## 7. 更多示例

```text
✨ feat(timing): 支持关闭单次强提醒
🐛 fix(schedule): 拒绝结束时间早于开始时间的日程
🔒️ fix(im): 禁止通过明文 HTTP 发送设备凭据
✅ test(application): 覆盖重复 request_id 的幂等创建
🔨 build(tooling): 迁移 Profile 驱动的固件打包工具
📝 docs(readme): 补充飞书适配器迁移路径
⬆️ build(deps): 升级 ESP-IDF 到 6.0.2
🔥 refactor(voice): 删除旧 Application 音频入口
```

不合格：

```text
update code
✨ feat: 优化功能
🐛 fix(im): 修复 bug 和更新 README
🚀 feat: 新增日程
```

问题分别是：没有格式；描述空泛；混入两个意图；emoji 不在受控清单且与 Type 不匹配。

## 8. 自动检查

检查单条提交描述：

```bash
python3 scripts/check_commit_message.py --file .git/COMMIT_EDITMSG
```

检查分支提交：

```bash
python3 scripts/check_commit_message.py --range origin/main..HEAD
```

CI 会检查 PR 中的非 Merge Commit。脚本检查格式、emoji/type 搭配、中文主题、长度、空行和 Breaking Change 声明；内容是否真实、提交是否该拆分，仍由作者与 Reviewer 负责。
