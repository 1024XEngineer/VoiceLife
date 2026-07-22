# VoiceLife 声活

> 语音优先、IM 辅助的日程提醒工具。

把生活里的安排说出来，也把每次变化处理稳。

VoiceLife 声活帮助用户在通勤、走路、做饭或开会前后等不便手动操作的时刻，直接通过语音记下和查询日程。语音负责当下的快速交互，IM 负责提供完整、可回看的提醒与操作回执。

产品围绕一条连续体验展开：用户说出日程或小事，在约定的触发点收到提醒，随后可以关闭或推迟当前提醒；当安排发生变化时，也能安全地修改单次日程或周期事项。

VoiceLife 支持：

- 用语音创建、查询单次日程和简单周期日程；
- 通过语音与 IM 接收提醒，并关闭或推迟当前提醒；
- 修改已有日程，明确控制周期变更影响“本次”“本次及以后”或“整个系列”。

## 可运行原型

本分支包含一个 Node.js + TypeScript 原型。日程和提醒数据保存在本机 SQLite，通过 MCP 提供给语音 Agent；可选的 macOS 客户端能够接收 TTS 音频并通过电脑扬声器播放到期提醒。

### 快速开始

要求 Node.js 24 或更新版本。

```bash
cp .env.example .env
npm install
npm run dev
```

打开 <http://localhost:3000> 查看 IM 回执模拟页。

首次运行前请修改 `.env` 中的 `MCP_SHARED_SECRET`。如需连接灵矽 MCP Proxy，在本机 `.env` 中填写 `LINX_API_KEY`。所有密钥和本地 SQLite 数据都不应提交到 Git。

## 常用命令

| 命令 | 用途 |
|---|---|
| `npm run dev` | 启动本地服务并监听源码变更 |
| `npm run build` | 编译生产代码到 `dist/` |
| `npm run start` | 运行已编译的服务 |
| `npm run typecheck` | 执行 TypeScript 类型检查 |
| `npm test` | 运行自动化测试 |
| `npm run linx:copy-token` | 将 MCP Proxy 的 `X-MCP-Token` 复制到剪贴板 |
| `npm run linx:voice:activate` | 激活并绑定 Mac 虚拟语音设备 |
| `npm run linx:voice:test` | 测试灵矽 TTS 与 macOS 扬声器链路 |

## 项目结构

```text
src/
  adapters/    外部平台与主动语音适配器
  clients/     灵矽设备 WebSocket、OTA 激活和音频播放客户端
  domain/      日程、周期和提醒领域模型
  http/        MCP、IM 与本地调试 HTTP 接口
  mcp/         面向语音 Agent 的日程工具
  services/    日程、变更、提醒、临时记录、时钟和回执服务
  storage/     SQLite 持久化
public/        IM 回执模拟页
tests/         自动化测试
docs/          原型说明和 Agent 配置文档
```

## 文档

- [原型说明](docs/prototype.md)：产品行为、交互闭环、接入步骤、验收场景和能力边界。
- [灵矽 Agent 提示词](docs/linx-agent-prompt.md)：可复制到 Agent 角色配置的工具调用规则。
- [PR #59 审查与实现决策](docs/proposal-59-implementation.md)：Proposal 覆盖情况、原型默认决策和待人工验证项。

## 验证

```bash
npm run typecheck
npm test
npm run build
git diff --check
```

当前代码用于快速原型验证，不代表生产部署方案。
