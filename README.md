# XE6-15

XE6-15 是一个围绕个人时间管理展开的产品探索仓库。当前增量聚焦语音日程助手：以语音作为主要操作入口，以 IM 保存可回看的操作回执，并验证创建/查询、提醒处理和日程变更的连续闭环。

本分支包含一个可运行的 Node.js + TypeScript 原型。日程和提醒数据保存在本机 SQLite，通过 MCP 提供给语音 Agent；可选的 macOS 客户端能够接收 TTS 音频并通过电脑扬声器播放到期提醒。

## 快速开始

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
