import { createServer } from "node:http";
import type { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import {
  fetchLinxProxyCredentials,
  LinxMcpProxyTransport,
} from "./adapters/linx-mcp-proxy.js";
import { LinxMacProactiveVoiceAdapter } from "./adapters/linx-mac-proactive-voice.js";
import { UnsupportedProactiveVoiceAdapter } from "./adapters/proactive-voice.js";
import type { ProactiveVoiceAdapter } from "./adapters/proactive-voice.js";
import { LinxMacVoiceClient } from "./clients/linx-mac-voice-client.js";
import { loadConfig } from "./config.js";
import { createApp } from "./http/app.js";
import { createCalendarMcpServer } from "./mcp/calendar-mcp.js";
import { CalendarService } from "./services/calendar-service.js";
import { CalendarMutationService } from "./services/calendar-mutation-service.js";
import { DemoClock, SystemClock } from "./services/clock.js";
import { ReceiptBus } from "./services/receipt-bus.js";
import { ReminderService } from "./services/reminder-service.js";
import { LinxSettingsService } from "./services/linx-settings-service.js";
import { ShortNoteService } from "./services/short-note-service.js";
import { CalendarDatabase } from "./storage/database.js";

const config = loadConfig();
const db = new CalendarDatabase(config.databasePath);
const receiptBus = new ReceiptBus();
const clock = config.demoMode
  ? new DemoClock(config.timeZone)
  : new SystemClock(config.timeZone);
const calendarService = new CalendarService(db, clock, receiptBus, config.timeZone);
const reconciledWeakReminders = calendarService.reconcileUpcomingWeakReminders();
if (reconciledWeakReminders > 0) {
  console.log(`Reconciled ${reconciledWeakReminders} upcoming weak reminder(s)`);
}
const mutationService = new CalendarMutationService(
  db,
  calendarService,
  clock,
  receiptBus,
  config.timeZone,
);
const shortNoteService = new ShortNoteService(db, clock, receiptBus);
const linxSettingsService = new LinxSettingsService();
const linxVoiceClient = config.linxVoice
  ? new LinxMacVoiceClient({
      webSocketUrl: config.linxVoice.webSocketUrl,
      token: config.linxVoice.token,
      deviceId: config.linxVoice.deviceId,
      clientId: config.linxVoice.clientId,
      agentId: config.linxVoice.agentId,
      voiceId: config.linxVoice.voiceId,
      timeoutMs: config.linxVoice.timeoutMs,
    })
  : undefined;
const proactiveVoice: ProactiveVoiceAdapter = linxVoiceClient
  ? new LinxMacProactiveVoiceAdapter(linxVoiceClient)
  : new UnsupportedProactiveVoiceAdapter();
const reminderService = new ReminderService(
  db,
  calendarService,
  clock,
  receiptBus,
  proactiveVoice,
);
const application = createApp({
  config,
  db,
  clock,
  receiptBus,
  calendarService,
  reminderService,
  mutationService,
  shortNoteService,
  voiceInteractor: linxVoiceClient,
  linxSettingsService,
});
const httpServer = createServer(application.app);
let linxProxyServer: McpServer | undefined;
let linxProxyTransport: LinxMcpProxyTransport | undefined;

if (config.linxApiKey) {
  try {
    const credentials = await fetchLinxProxyCredentials(config.linxApiKey);
    linxProxyTransport = new LinxMcpProxyTransport(credentials.webSocketUrl);
    linxProxyServer = createCalendarMcpServer(
      calendarService,
      reminderService,
      mutationService,
      shortNoteService,
    );
    await linxProxyServer.connect(linxProxyTransport);
    console.log(`Linx MCP Proxy connected; token expires at ${credentials.expiresAt}`);
  } catch (error) {
    console.error("Linx MCP Proxy connection failed", error);
  }
}

const scheduler = setInterval(() => {
  void reminderService.scanDue().catch((error) => console.error("Reminder scheduler failed", error));
}, config.schedulerIntervalMs);
scheduler.unref();

httpServer.listen(config.port, "0.0.0.0", () => {
  console.log(`Voice calendar prototype: http://localhost:${config.port}`);
  console.log(`MCP Streamable HTTP endpoint: http://localhost:${config.port}/mcp`);
  console.log(`MCP SSE endpoint: http://localhost:${config.port}/sse`);
  if (config.mcpSharedSecret === "dev-only-change-me") {
    console.warn("MCP_SHARED_SECRET is using the development default. Change it before opening a tunnel.");
  }
});

async function shutdown(): Promise<void> {
  clearInterval(scheduler);
  httpServer.close();
  await application.closeMcpSessions();
  await linxProxyServer?.close();
  await proactiveVoice.close?.();
  db.close();
}

process.on("SIGINT", () => void shutdown().finally(() => process.exit(0)));
process.on("SIGTERM", () => void shutdown().finally(() => process.exit(0)));
