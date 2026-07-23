import type { NextFunction, Request, Response } from "express";
import express from "express";
import { randomUUID } from "node:crypto";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { SSEServerTransport } from "@modelcontextprotocol/sdk/server/sse.js";
import type { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { StreamableHTTPServerTransport } from "@modelcontextprotocol/sdk/server/streamableHttp.js";
import { isInitializeRequest } from "@modelcontextprotocol/sdk/types.js";
import type { AppConfig } from "../config.js";
import { createCalendarMcpServer } from "../mcp/calendar-mcp.js";
import { CalendarDatabase } from "../storage/database.js";
import { CalendarService } from "../services/calendar-service.js";
import { CalendarMutationService } from "../services/calendar-mutation-service.js";
import { DemoClock, type Clock } from "../services/clock.js";
import { ReceiptBus } from "../services/receipt-bus.js";
import { ReminderService } from "../services/reminder-service.js";
import { LinxSettingsService } from "../services/linx-settings-service.js";
import { ShortNoteService } from "../services/short-note-service.js";

interface SseMcpSession {
  transport: SSEServerTransport;
  server: McpServer;
}

interface StreamableHttpMcpSession {
  transport: StreamableHTTPServerTransport;
  server: McpServer;
}

interface VoiceInteractor {
  speak(text: string, options?: {
    onSpokenText?: (text: string) => void | Promise<void>;
  }): Promise<{
    spokenText: string | null;
    audioBytes: number;
    format: "pcm" | "opus";
  }>;
}

export interface ApplicationDependencies {
  config: AppConfig;
  db: CalendarDatabase;
  clock: Clock;
  receiptBus: ReceiptBus;
  calendarService: CalendarService;
  reminderService: ReminderService;
  mutationService: CalendarMutationService;
  shortNoteService: ShortNoteService;
  voiceInteractor?: VoiceInteractor;
  linxSettingsService?: LinxSettingsService;
}

function isLocalRequest(req: Request): boolean {
  const host = (req.headers.host ?? "").split(":")[0];
  return host === "localhost" || host === "127.0.0.1" || host === "[::1]";
}

export function createApp(deps: ApplicationDependencies) {
  const app = express();
  const sseSessions = new Map<string, SseMcpSession>();
  const streamableHttpSessions = new Map<string, StreamableHttpMcpSession>();
  app.disable("x-powered-by");
  app.use(express.json({ limit: "128kb" }));

  const mcpAuth = (req: Request, res: Response, next: NextFunction) => {
    if (req.headers.authorization !== `Bearer ${deps.config.mcpSharedSecret}`) {
      res.status(401).json({ error: "unauthorized" });
      return;
    }
    next();
  };

  app.get("/health", (_req, res) => {
    res.json({ ok: true, now: deps.clock.now().toISO(), demoMode: deps.config.demoMode });
  });

  app.get("/sse", mcpAuth, async (_req, res, next) => {
    try {
      const transport = new SSEServerTransport("/messages", res);
      const server = createCalendarMcpServer(
        deps.calendarService,
        deps.reminderService,
        deps.mutationService,
        deps.shortNoteService,
      );
      const sessionId = transport.sessionId;
      sseSessions.set(sessionId, { transport, server });
      transport.onclose = () => sseSessions.delete(sessionId);
      transport.onerror = (error) => console.error("MCP SSE transport error", error);
      await server.connect(transport);
    } catch (error) {
      next(error);
    }
  });

  app.post("/messages", mcpAuth, async (req, res) => {
    const sessionId = typeof req.query.sessionId === "string" ? req.query.sessionId : "";
    const session = sseSessions.get(sessionId);
    if (!session) {
      res.status(404).json({ error: "unknown MCP session" });
      return;
    }
    await session.transport.handlePostMessage(req, res, req.body);
  });

  app.all("/mcp", mcpAuth, async (req, res, next) => {
    try {
      const sessionId = typeof req.headers["mcp-session-id"] === "string"
        ? req.headers["mcp-session-id"]
        : "";
      let session = sessionId ? streamableHttpSessions.get(sessionId) : undefined;

      if (!session && !sessionId && req.method === "POST" && isInitializeRequest(req.body)) {
        const server = createCalendarMcpServer(
          deps.calendarService,
          deps.reminderService,
          deps.mutationService,
          deps.shortNoteService,
        );
        const transport = new StreamableHTTPServerTransport({
          sessionIdGenerator: () => randomUUID(),
          enableJsonResponse: true,
          onsessioninitialized: (initializedSessionId) => {
            streamableHttpSessions.set(initializedSessionId, { transport, server });
          },
        });
        transport.onclose = () => {
          const initializedSessionId = transport.sessionId;
          if (initializedSessionId) streamableHttpSessions.delete(initializedSessionId);
        };
        transport.onerror = (error) => console.error("MCP Streamable HTTP transport error", error);
        await server.connect(transport);
        session = { transport, server };
      }

      if (!session) {
        res.status(sessionId ? 404 : 400).json({
          jsonrpc: "2.0",
          error: {
            code: -32000,
            message: sessionId
              ? "Unknown MCP session"
              : "Missing MCP session or initialize request",
          },
          id: null,
        });
        return;
      }

      await session.transport.handleRequest(req, res, req.body);
    } catch (error) {
      next(error);
    }
  });

  app.use((req, res, next) => {
    if (isLocalRequest(req)) {
      next();
      return;
    }
    res.status(404).end();
  });

  app.post("/api/voice/interact", async (req, res) => {
    if (!deps.voiceInteractor) {
      res.status(503).json({ error: "语音服务尚未连接，请先完成 Mac 语音设备绑定" });
      return;
    }

    const text = typeof req.body.text === "string" ? req.body.text.trim() : "";
    if (!text) {
      res.status(400).json({ error: "没有识别到可发送的语音内容" });
      return;
    }
    if (text.length > 500) {
      res.status(400).json({ error: "单次语音内容不能超过 500 个字符" });
      return;
    }

    try {
      const result = await deps.voiceInteractor.speak(text);
      res.json({
        ok: true,
        reply: result.spokenText,
        audioBytes: result.audioBytes,
        format: result.format,
      });
    } catch (error) {
      const message = error instanceof Error ? error.message : "语音交互失败";
      res.status(502).json({ error: message });
    }
  });

  app.post("/api/voice/interact/stream", async (req, res) => {
    if (!deps.voiceInteractor) {
      res.status(503).json({ error: "语音服务尚未连接，请先完成 Mac 语音设备绑定" });
      return;
    }

    const text = typeof req.body.text === "string" ? req.body.text.trim() : "";
    if (!text) {
      res.status(400).json({ error: "没有识别到可发送的语音内容" });
      return;
    }
    if (text.length > 500) {
      res.status(400).json({ error: "单次语音内容不能超过 500 个字符" });
      return;
    }

    res.status(200);
    res.setHeader("Content-Type", "application/x-ndjson; charset=utf-8");
    res.setHeader("Cache-Control", "no-cache, no-transform");
    res.setHeader("X-Accel-Buffering", "no");
    res.flushHeaders();

    const receiptIdsBefore = new Set(deps.db.listReceipts(200).map((receipt) => receipt.id));
    const sentReceiptIds = new Set<string>();
    let sentText = false;
    const writeEvent = (event: Record<string, unknown>) => {
      if (!res.writableEnded) res.write(`${JSON.stringify(event)}\n`);
    };

    try {
      const result = await deps.voiceInteractor.speak(text, {
        onSpokenText: async (spokenText) => {
          sentText = true;
          const queryReceipt = [...deps.db.listReceipts(200)]
            .reverse()
            .find((receipt) =>
              receipt.type === "calendar_query"
              && !receiptIdsBefore.has(receipt.id)
              && !sentReceiptIds.has(receipt.id));
          if (queryReceipt) sentReceiptIds.add(queryReceipt.id);
          writeEvent({ type: "message", text: spokenText, receipt: queryReceipt ?? null });
          await new Promise((resolve) => setTimeout(resolve, 220));
        },
      });
      if (!sentText && result.spokenText) {
        const queryReceipt = [...deps.db.listReceipts(200)]
          .reverse()
          .find((receipt) => receipt.type === "calendar_query" && !receiptIdsBefore.has(receipt.id));
        writeEvent({ type: "message", text: result.spokenText, receipt: queryReceipt ?? null });
      }
      writeEvent({
        type: "complete",
        audioBytes: result.audioBytes,
        format: result.format,
      });
    } catch (error) {
      writeEvent({
        type: "error",
        error: error instanceof Error ? error.message : "语音交互失败",
      });
    } finally {
      res.end();
    }
  });

  app.get("/api/settings/linx", async (_req, res) => {
    if (!deps.linxSettingsService) {
      res.status(503).json({ error: "灵矽设置服务尚未启用" });
      return;
    }
    const status = await deps.linxSettingsService.getStatus();
    res.json({ ...status, runtimeVoiceReady: Boolean(deps.voiceInteractor) });
  });

  app.put("/api/settings/linx", async (req, res) => {
    if (!deps.linxSettingsService) {
      res.status(503).json({ error: "灵矽设置服务尚未启用" });
      return;
    }
    const status = await deps.linxSettingsService.save({
      apiKey: req.body.apiKey,
      agentId: req.body.agentId,
      voiceId: req.body.voiceId,
    });
    res.json({ ...status, restartRequired: true });
  });

  app.post("/api/settings/linx/mcp-token", async (_req, res) => {
    if (!deps.linxSettingsService) {
      res.status(503).json({ error: "灵矽设置服务尚未启用" });
      return;
    }
    res.json(await deps.linxSettingsService.createMcpToken());
  });

  app.post("/api/settings/linx/activate", async (_req, res) => {
    if (!deps.linxSettingsService) {
      res.status(503).json({ error: "灵矽设置服务尚未启用" });
      return;
    }
    res.json({
      ...await deps.linxSettingsService.activateDevice(),
      restartRequired: true,
    });
  });

  app.post("/api/settings/linx/test", async (_req, res) => {
    if (!deps.linxSettingsService) {
      res.status(503).json({ error: "灵矽设置服务尚未启用" });
      return;
    }
    res.json({ ok: true, ...await deps.linxSettingsService.testVoice() });
  });

  app.get("/api/receipts", (_req, res) => {
    res.json({ receipts: deps.db.listReceipts(200) });
  });

  app.get("/api/receipts/stream", (req, res) => {
    res.setHeader("Content-Type", "text/event-stream");
    res.setHeader("Cache-Control", "no-cache");
    res.setHeader("Connection", "keep-alive");
    res.flushHeaders();
    res.write(": connected\n\n");
    const unsubscribe = deps.receiptBus.subscribe((receipt) => {
      res.write(`event: receipt\ndata: ${JSON.stringify(receipt)}\n\n`);
    });
    const keepAlive = setInterval(() => res.write(": keep-alive\n\n"), 15_000);
    req.on("close", () => {
      clearInterval(keepAlive);
      unsubscribe();
    });
  });

  app.post("/api/reminders/:id/close", (req, res) => {
    res.json(deps.reminderService.close(req.params.id));
  });

  app.post("/api/reminders/:id/snooze", (req, res) => {
    res.json(deps.reminderService.snooze(req.params.id, Number(req.body.minutes)));
  });

  app.post("/api/calendar/undo/:id", (req, res) => {
    res.json(deps.mutationService.undo(req.params.id));
  });

  app.get("/api/demo/clock", (_req, res) => {
    res.json({ now: deps.clock.now().toISO(), enabled: deps.clock instanceof DemoClock });
  });

  app.post("/api/demo/clock/set", async (req, res) => {
    if (!(deps.clock instanceof DemoClock)) {
      res.status(404).json({ error: "demo clock disabled" });
      return;
    }
    const now = deps.clock.set(String(req.body.iso));
    const pushed = await deps.reminderService.scanDue();
    res.json({ now: now.toISO(), pushed: pushed.length });
  });

  app.post("/api/demo/clock/advance", async (req, res) => {
    if (!(deps.clock instanceof DemoClock)) {
      res.status(404).json({ error: "demo clock disabled" });
      return;
    }
    const now = deps.clock.advance(Number(req.body.minutes));
    const pushed = await deps.reminderService.scanDue();
    res.json({ now: now.toISO(), pushed: pushed.length });
  });

  app.post("/api/demo/clock/reset", (req, res) => {
    if (!(deps.clock instanceof DemoClock)) {
      res.status(404).json({ error: "demo clock disabled" });
      return;
    }
    res.json({ now: deps.clock.reset().toISO() });
  });

  const currentDir = path.dirname(fileURLToPath(import.meta.url));
  const publicDir = path.resolve(currentDir, "../../public");
  app.use(express.static(publicDir));

  app.use((error: unknown, _req: Request, res: Response, _next: NextFunction) => {
    const message = error instanceof Error ? error.message : "unknown error";
    res.status(400).json({ error: message });
  });

  return {
    app,
    async closeMcpSessions() {
      await Promise.all(
        [...sseSessions.values(), ...streamableHttpSessions.values()].map(async ({ server }) => {
          await server.close();
        }),
      );
      sseSessions.clear();
      streamableHttpSessions.clear();
    },
  };
}
