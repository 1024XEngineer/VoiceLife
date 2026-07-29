const TOOLS = [
  {
    name: "voicelife.create_binding_code",
    description: "生成微信公众号绑定码。用户可把绑定码发送给 VoiceLife 公众号，将当前小智设备与微信绑定。",
    inputSchema: { type: "object", properties: {}, additionalProperties: false }
  },
  {
    name: "voicelife.create_reminder",
    description: "为当前小智设备创建提醒。时间必须是带时区的 ISO 8601 格式；中国时间使用 +08:00。",
    inputSchema: {
      type: "object",
      properties: {
        title: { type: "string", description: "提醒内容" },
        due_at: {
          type: "string",
          description: "带时区的 ISO 8601 时间，例如 2026-07-30T15:00:00+08:00"
        }
      },
      required: ["title", "due_at"],
      additionalProperties: false
    }
  },
  {
    name: "voicelife.list_reminders",
    description: "查询当前小智设备尚未关闭的提醒。",
    inputSchema: { type: "object", properties: {}, additionalProperties: false }
  },
  {
    name: "voicelife.dismiss_reminder",
    description: "关闭当前小智设备的指定提醒；不提供 reminder_id 时关闭最近一条待处理提醒。",
    inputSchema: {
      type: "object",
      properties: {
        reminder_id: { type: "string", description: "可选的提醒 ID" }
      },
      additionalProperties: false
    }
  },
  {
    name: "voicelife.snooze_reminder",
    description: "推迟当前小智设备的指定提醒；默认推迟 10 分钟。",
    inputSchema: {
      type: "object",
      properties: {
        reminder_id: { type: "string", description: "可选的提醒 ID" },
        minutes: { type: "integer", minimum: 1, maximum: 1440 }
      },
      additionalProperties: false
    }
  }
];

async function messageText(data) {
  if (typeof data === "string") return data;
  if (Buffer.isBuffer(data)) return data.toString("utf8");
  if (data instanceof ArrayBuffer) return Buffer.from(data).toString("utf8");
  if (ArrayBuffer.isView(data)) {
    return Buffer.from(data.buffer, data.byteOffset, data.byteLength).toString("utf8");
  }
  if (data && typeof data.text === "function") return data.text();
  return String(data);
}

function closeReason(event) {
  const reason = event.reason || "无";
  return `code=${event.code ?? "未知"} reason=${reason} clean=${event.wasClean ?? "未知"}`;
}

function errorSummary(event) {
  const error = event?.error ?? event;
  if (!error) return "未知 WebSocket 错误";
  const parts = [error.name, error.message, error.code, error.cause?.code]
    .filter(Boolean);
  return parts.length ? parts.join(": ") : String(error);
}

function endpointLabel(endpoint) {
  try {
    const url = new URL(endpoint);
    return `${url.protocol}//${url.host}${url.pathname}`;
  } catch {
    return "无效地址";
  }
}

function result(id, text, isError = false) {
  return {
    jsonrpc: "2.0",
    id,
    result: {
      content: [{ type: "text", text }],
      isError
    }
  };
}

export class XiaozhiMcpBridge {
  constructor({
    endpoint,
    deviceId,
    service,
    WebSocketImpl = globalThis.WebSocket,
    logger = console,
    reconnectMs = 2000,
    maxReconnectMs = 60_000,
    debug = false
  }) {
    this.endpoint = endpoint;
    this.deviceId = deviceId;
    this.service = service;
    this.WebSocketImpl = WebSocketImpl;
    this.logger = logger;
    this.reconnectMs = reconnectMs;
    this.maxReconnectMs = maxReconnectMs;
    this.debug = debug;
    this.socket = null;
    this.reconnectTimer = null;
    this.stopped = true;
    this.reconnectAttempt = 0;
    this.connectedAt = 0;
    this.receivedMessages = 0;
    this.lastMethod = "";
  }

  start() {
    if (!this.endpoint) {
      this.logger.info("[xiaozhi] 未配置 XIAOZHI_MCP_ENDPOINT，跳过 MCP 连接");
      return;
    }
    if (!this.WebSocketImpl) throw new Error("当前 Node.js 没有 WebSocket 实现");
    this.stopped = false;
    this.connect();
  }

  connect() {
    if (this.stopped) return;
    this.logger.info(`[xiaozhi] 正在连接 MCP 接入点 ${endpointLabel(this.endpoint)}`);
    const socket = new this.WebSocketImpl(this.endpoint);
    this.socket = socket;
    socket.addEventListener("open", () => {
      this.connectedAt = Date.now();
      this.receivedMessages = 0;
      this.lastMethod = "";
      this.logger.info(`[xiaozhi] MCP 已连接，device=${this.deviceId}`);
      this.send({
        jsonrpc: "2.0",
        method: "notifications/tools/list_changed"
      });
    });
    socket.addEventListener("message", (event) => {
      messageText(event.data)
        .then((raw) => this.onMessage(raw))
        .catch((error) => this.logger.error("[xiaozhi] MCP 消息处理失败", error));
    });
    socket.addEventListener("error", (event) => {
      this.logger.error(`[xiaozhi] MCP WebSocket 错误：${errorSummary(event)}`);
    });
    socket.addEventListener("close", (event) => {
      const connectedMs = this.connectedAt ? Date.now() - this.connectedAt : 0;
      this.logger.info(
        `[xiaozhi] MCP 已断开：${closeReason(event)} ` +
        `connectedMs=${connectedMs} messages=${this.receivedMessages} ` +
        `lastMethod=${this.lastMethod || "无"}`
      );
      this.socket = null;
      if (!this.stopped) {
        if (connectedMs >= 60_000) this.reconnectAttempt = 0;
        else this.reconnectAttempt += 1;
        const delay = Math.min(
          this.maxReconnectMs,
          this.reconnectMs * 2 ** Math.max(0, this.reconnectAttempt - 1)
        );
        this.logger.info(`[xiaozhi] ${delay}ms 后重连（第 ${this.reconnectAttempt} 次）`);
        this.reconnectTimer = setTimeout(() => this.connect(), delay);
      }
    });
  }

  stop() {
    this.stopped = true;
    clearTimeout(this.reconnectTimer);
    this.socket?.close();
  }

  send(payload) {
    const open = this.WebSocketImpl.OPEN ?? 1;
    if (!this.socket || this.socket.readyState !== open) return;
    this.socket.send(JSON.stringify(payload));
  }

  async onMessage(raw) {
    const request = JSON.parse(raw);
    if (!request.method) return;
    this.receivedMessages += 1;
    this.lastMethod = request.method;
    if (this.debug) this.logger.info(`[xiaozhi] ← ${request.method}`);
    if (request.method === "initialize") {
      this.send({
        jsonrpc: "2.0",
        id: request.id,
        result: {
          protocolVersion: "2024-11-05",
          capabilities: { tools: {} },
          serverInfo: { name: "voicelife-wechat-demo", version: "0.1.0" }
        }
      });
      return;
    }
    if (request.method === "ping") {
      this.send({ jsonrpc: "2.0", id: request.id, result: {} });
      return;
    }
    if (request.method === "tools/list") {
      this.send({ jsonrpc: "2.0", id: request.id, result: { tools: TOOLS } });
      return;
    }
    if (request.method === "tools/call") {
      try {
        const text = await this.callTool(
          request.params?.name,
          request.params?.arguments ?? {}
        );
        this.send(result(request.id, text));
      } catch (error) {
        this.send(result(request.id, `操作失败：${error.message}`, true));
      }
      return;
    }
    if (request.id !== undefined) {
      this.send({
        jsonrpc: "2.0",
        id: request.id,
        error: { code: -32601, message: `Method not found: ${request.method}` }
      });
    }
  }

  async callTool(name, args) {
    switch (name) {
      case "voicelife.create_binding_code": {
        const binding = this.service.createBindingCode(this.deviceId);
        return `微信绑定码是 ${binding.code}，请在十分钟内发送“绑定 ${binding.code}”给 VoiceLife 公众号。`;
      }
      case "voicelife.create_reminder": {
        const reminder = this.service.createReminder({
          deviceId: this.deviceId,
          title: args.title,
          dueAt: args.due_at
        });
        return `提醒已创建：${reminder.title}，时间 ${reminder.dueAt}，编号 ${reminder.id}`;
      }
      case "voicelife.list_reminders": {
        const reminders = this.service.listReminders(this.deviceId);
        if (!reminders.length) return "当前没有待处理提醒。";
        return reminders
          .map((item) => `${item.title}，${item.dueAt}，编号 ${item.id}`)
          .join("\n");
      }
      case "voicelife.dismiss_reminder": {
        const reminder = this.service.dismissForDevice({
          deviceId: this.deviceId,
          reminderId: args.reminder_id
        });
        return `已关闭提醒：${reminder.title}`;
      }
      case "voicelife.snooze_reminder": {
        const reminder = this.service.snoozeForDevice({
          deviceId: this.deviceId,
          reminderId: args.reminder_id,
          minutes: args.minutes ?? 10
        });
        return `已推迟提醒：${reminder.title}，新的时间是 ${reminder.dueAt}`;
      }
      default:
        throw new Error(`未知工具：${name}`);
    }
  }
}

export { TOOLS as xiaozhiTools };
