import assert from "node:assert/strict";
import test from "node:test";
import { VoiceLifeService } from "../src/service.js";
import { MemoryStore } from "../src/store.js";
import { XiaozhiMcpBridge, xiaozhiTools } from "../src/xiaozhi-mcp.js";

test("小智 MCP 暴露完整的提醒工具", () => {
  assert.deepEqual(
    xiaozhiTools.map((tool) => tool.name),
    [
      "voicelife.create_binding_code",
      "voicelife.create_reminder",
      "voicelife.list_reminders",
      "voicelife.dismiss_reminder",
      "voicelife.snooze_reminder"
    ]
  );
});

test("小智 MCP 工具可以创建和查询提醒", async () => {
  const service = new VoiceLifeService({
    store: new MemoryStore(),
    wechatApi: {},
    now: () => Date.parse("2026-07-29T10:00:00+08:00")
  });
  const bridge = new XiaozhiMcpBridge({
    endpoint: "",
    deviceId: "xiaozhi-01",
    service,
    logger: { info() {}, error() {} }
  });
  const created = await bridge.callTool("voicelife.create_reminder", {
    title: "喝水",
    due_at: "2026-07-29T10:05:00+08:00"
  });
  assert.match(created, /提醒已创建：喝水/);
  assert.match(await bridge.callTool("voicelife.list_reminders", {}), /喝水/);
});

test("小智 MCP ping 返回空结果用于保持连接", async () => {
  const sent = [];
  const WebSocketImpl = { OPEN: 1 };
  const bridge = new XiaozhiMcpBridge({
    endpoint: "",
    deviceId: "xiaozhi-01",
    service: {},
    WebSocketImpl,
    logger: { info() {}, error() {} }
  });
  bridge.socket = {
    readyState: 1,
    send(value) {
      sent.push(JSON.parse(value));
    }
  };
  await bridge.onMessage(JSON.stringify({
    jsonrpc: "2.0",
    id: 9,
    method: "ping"
  }));
  assert.deepEqual(sent, [{ jsonrpc: "2.0", id: 9, result: {} }]);
});
