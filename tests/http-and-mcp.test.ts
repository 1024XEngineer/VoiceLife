import { createServer, type Server } from "node:http";
import { mkdtemp, readFile, rm, writeFile } from "node:fs/promises";
import { AddressInfo } from "node:net";
import { tmpdir } from "node:os";
import path from "node:path";
import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { SSEClientTransport } from "@modelcontextprotocol/sdk/client/sse.js";
import { StreamableHTTPClientTransport } from "@modelcontextprotocol/sdk/client/streamableHttp.js";
import request from "supertest";
import { afterEach, beforeEach, describe, expect, it } from "vitest";
import type { AppConfig } from "../src/config.js";
import { createApp } from "../src/http/app.js";
import { LinxSettingsService } from "../src/services/linx-settings-service.js";
import { createTestServices } from "./helpers.js";

describe("HTTP and MCP integration", () => {
  let services: ReturnType<typeof createTestServices>;
  let application: ReturnType<typeof createApp>;
  let config: AppConfig;

  beforeEach(() => {
    services = createTestServices();
    config = {
      port: 0,
      databasePath: ":memory:",
      timeZone: services.timeZone,
      mcpSharedSecret: "test-secret",
      demoMode: true,
      schedulerIntervalMs: 1000,
    };
    application = createApp({ config, ...services });
  });

  afterEach(async () => {
    await application.closeMcpSessions();
    services.db.close();
  });

  it("protects MCP and keeps the voice prototype local", async () => {
    await request(application.app).get("/sse").expect(401);
    await request(application.app).post("/mcp").send({}).expect(401);
    await request(application.app).get("/").set("Host", "example.test").expect(404);
    const page = await request(application.app).get("/").set("Host", "localhost").expect(200);
    expect(page.text).toContain("按住说话");
    expect(page.text).toContain("消息回执");
    expect(page.text).toContain("/settings.html");
    expect(page.text).not.toContain("DEMO CLOCK");
    const settingsPage = await request(application.app)
      .get("/settings.html")
      .set("Host", "localhost")
      .expect(200);
    expect(settingsPage.text).toContain("接入教程");
    await request(application.app)
      .get("/settings.html")
      .set("Host", "example.test")
      .expect(404);
  });

  it("stores Linx settings locally without returning the API key", async () => {
    const directory = await mkdtemp(path.join(tmpdir(), "linx-settings-test-"));
    const envPath = path.join(directory, ".env");
    const examplePath = path.join(directory, ".env.example");
    await writeFile(examplePath, "LINX_API_KEY=\nLINX_AGENT_ID=\nLINX_VOICE_ID=\n", "utf8");
    await writeFile(envPath, "LINX_API_KEY=old-secret\nLINX_AGENT_ID=old-agent\n", "utf8");
    const settingsApplication = createApp({
      config,
      ...services,
      linxSettingsService: new LinxSettingsService(envPath, examplePath),
    });

    try {
      const before = await request(settingsApplication.app)
        .get("/api/settings/linx")
        .set("Host", "localhost")
        .expect(200);
      expect(before.body).toMatchObject({
        apiKeyConfigured: true,
        agentId: "old-agent",
      });
      expect(JSON.stringify(before.body)).not.toContain("old-secret");

      const saved = await request(settingsApplication.app)
        .put("/api/settings/linx")
        .set("Host", "localhost")
        .send({ apiKey: "new-secret", agentId: "calendar-agent", voiceId: "warm-voice" })
        .expect(200);
      expect(saved.body).toMatchObject({
        apiKeyConfigured: true,
        agentId: "calendar-agent",
        voiceId: "warm-voice",
        restartRequired: true,
      });
      expect(JSON.stringify(saved.body)).not.toContain("new-secret");
      const stored = await readFile(envPath, "utf8");
      expect(stored).toContain('LINX_API_KEY="new-secret"');
      expect(stored).toContain('LINX_AGENT_ID="calendar-agent"');
      expect(stored).toContain('LINX_VOICE_ID="warm-voice"');
    } finally {
      await settingsApplication.closeMcpSessions();
      await rm(directory, { recursive: true, force: true });
    }
  });

  it("reports when the local voice client is not configured", async () => {
    const response = await request(application.app)
      .post("/api/voice/interact")
      .set("Host", "localhost")
      .send({ text: "今晚七点提醒我写日报" })
      .expect(503);
    expect(response.body.error).toContain("语音服务尚未连接");
  });

  it("sends recognized text to the bound voice Agent", async () => {
    let receivedText = "";
    const voiceApplication = createApp({
      config,
      ...services,
      voiceInteractor: {
        async speak(text) {
          receivedText = text;
          return { spokenText: "已为你创建今晚七点的日报提醒", audioBytes: 1024, format: "pcm" };
        },
      },
    });

    try {
      const response = await request(voiceApplication.app)
        .post("/api/voice/interact")
        .set("Host", "localhost")
        .send({ text: "  今晚七点提醒我写日报  " })
        .expect(200);
      expect(receivedText).toBe("今晚七点提醒我写日报");
      expect(response.body).toMatchObject({
        ok: true,
        reply: "已为你创建今晚七点的日报提醒",
        audioBytes: 1024,
        format: "pcm",
      });
    } finally {
      await voiceApplication.closeMcpSessions();
    }
  });

  it("exposes the integrated proposal tools over authenticated Streamable HTTP MCP", async () => {
    const httpServer: Server = createServer(application.app);
    await new Promise<void>((resolve) => httpServer.listen(0, "127.0.0.1", resolve));
    const address = httpServer.address() as AddressInfo;
    const transport = new StreamableHTTPClientTransport(
      new URL(`http://127.0.0.1:${address.port}/mcp`),
      {
        requestInit: {
          headers: { Authorization: `Bearer ${config.mcpSharedSecret}` },
        },
      },
    );
    const client = new Client({ name: "streamable-http-test", version: "1.0.0" });

    try {
      await client.connect(transport);
      const tools = await client.listTools();
      expect(tools.tools.map((tool) => tool.name)).toEqual([
        "calendar_create",
        "calendar_query",
        "calendar_find",
        "calendar_reschedule_occurrence",
        "reminder_list_due",
        "reminder_close",
        "reminder_snooze",
        "reminder_get_details",
        "note_record",
        "note_query",
        "calendar_modify",
        "calendar_skip_occurrence",
        "calendar_pause_series",
        "calendar_resume_series",
        "calendar_terminate_series",
        "calendar_delete",
        "calendar_undo",
      ]);
      const calendarQuery = tools.tools.find((tool) => tool.name === "calendar_query");
      expect(calendarQuery?.description).toContain("‘今天’是今天 00:00");
      expect(calendarQuery?.description).toContain("禁止把‘今天’查询成现在起未来 24 小时");
      expect(calendarQuery?.inputSchema.properties).toMatchObject({
        rangeStart: { description: expect.stringContaining("自然日的 00:00") },
        rangeEnd: { description: expect.stringContaining("不得使用当前时间加 24 小时") },
      });
      const calendarCreate = tools.tools.find((tool) => tool.name === "calendar_create");
      expect(calendarCreate?.description).toContain("首次调用省略 conflictConfirmationToken");
      expect(calendarCreate?.inputSchema.properties).toMatchObject({
        conflictConfirmationToken: { description: expect.stringContaining("用户明确确认") },
      });

      await client.callTool({
        name: "calendar_create",
        arguments: { title: "开会", startsAt: "2026-07-21T11:00:00+08:00" },
      });
      const blocked = await client.callTool({
        name: "calendar_create",
        arguments: { title: "吃饭", startsAt: "2026-07-21T11:00:00+08:00" },
      });
      expect(blocked.structuredContent).toMatchObject({
        ok: false,
        reason: "calendar_conflict",
        requiresConfirmation: true,
        requestedTitle: "吃饭",
      });
      const blockedContent = blocked.structuredContent as {
        conflictConfirmationToken: string;
      };
      expect(blockedContent.conflictConfirmationToken).toMatch(/^[a-f0-9]{64}$/);
      expect(services.db.listEvents()).toHaveLength(1);

      const confirmed = await client.callTool({
        name: "calendar_create",
        arguments: {
          title: "吃饭",
          startsAt: "2026-07-21T11:00:00+08:00",
          conflictConfirmationToken: blockedContent.conflictConfirmationToken,
        },
      });
      expect(confirmed.structuredContent).toMatchObject({
        ok: true,
        conflictConfirmed: true,
      });
      expect(services.db.listEvents()).toHaveLength(2);
    } finally {
      await client.close();
      await new Promise<void>((resolve, reject) =>
        httpServer.close((error) => (error ? reject(error) : resolve())),
      );
    }
  });

  it("uses the same reminder operations from the IM HTTP API", async () => {
    const created = services.calendarService.create({
      title: "吃药",
      startsAt: "2026-07-21T09:01:00+08:00",
    });
    services.clock.advance(1);
    await services.reminderService.scanDue();

    const snoozed = await request(application.app)
      .post(`/api/reminders/${created.reminder.id}/snooze`)
      .set("Host", "localhost")
      .send({ minutes: 10 })
      .expect(200);
    expect(snoozed.body.reminder.status).toBe("snoozed");
    expect(services.calendarService.getEvent(created.event.id)!.startAt).toBe(created.event.startAt);
  });

  it("exposes the integrated proposal tools over authenticated legacy SSE MCP", async () => {
    const httpServer: Server = createServer(application.app);
    await new Promise<void>((resolve) => httpServer.listen(0, "127.0.0.1", resolve));
    const address = httpServer.address() as AddressInfo;
    const headers = { Authorization: `Bearer ${config.mcpSharedSecret}` };
    const transport = new SSEClientTransport(
      new URL(`http://127.0.0.1:${address.port}/sse`),
      { requestInit: { headers } },
    );
    const client = new Client({ name: "prototype-test", version: "1.0.0" });

    try {
      await client.connect(transport);
      const tools = await client.listTools();
      expect(tools.tools.map((tool) => tool.name)).toEqual([
        "calendar_create",
        "calendar_query",
        "calendar_find",
        "calendar_reschedule_occurrence",
        "reminder_list_due",
        "reminder_close",
        "reminder_snooze",
        "reminder_get_details",
        "note_record",
        "note_query",
        "calendar_modify",
        "calendar_skip_occurrence",
        "calendar_pause_series",
        "calendar_resume_series",
        "calendar_terminate_series",
        "calendar_delete",
        "calendar_undo",
      ]);

      const response = await client.callTool({
        name: "calendar_create",
        arguments: {
          title: "客户拜访",
          startsAt: "2026-07-22T10:00:00+08:00",
          location: "A 座 1503",
        },
      });
      expect(response.isError).not.toBe(true);
      expect(response.structuredContent).toMatchObject({ ok: true });
      expect(services.db.listReceipts()).toHaveLength(1);
    } finally {
      await client.close();
      await new Promise<void>((resolve, reject) =>
        httpServer.close((error) => (error ? reject(error) : resolve())),
      );
    }
  });
});
