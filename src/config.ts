import "dotenv/config";
import path from "node:path";

export interface AppConfig {
  port: number;
  databasePath: string;
  timeZone: string;
  mcpSharedSecret: string;
  linxApiKey?: string;
  demoMode: boolean;
  schedulerIntervalMs: number;
  linxVoice?: LinxVoiceConfig;
}

export interface LinxVoiceConfig {
  webSocketUrl: string;
  token: string;
  deviceId: string;
  clientId: string;
  agentId?: string;
  voiceId?: string;
  timeoutMs: number;
}

function positiveInteger(value: string | undefined, fallback: number): number {
  if (!value) return fallback;
  const parsed = Number.parseInt(value, 10);
  return Number.isFinite(parsed) && parsed > 0 ? parsed : fallback;
}

function loadLinxVoiceConfig(env: NodeJS.ProcessEnv): LinxVoiceConfig | undefined {
  if ((env.LINX_PROACTIVE_VOICE ?? "false").toLowerCase() !== "true") return undefined;
  const required = {
    token: env.LINX_DEVICE_WS_TOKEN,
    deviceId: env.LINX_DEVICE_ID,
    clientId: env.LINX_DEVICE_CLIENT_ID,
  };
  const missing = Object.entries(required)
    .filter(([, value]) => !value)
    .map(([key]) => key);
  if (missing.length > 0) {
    throw new Error(`已启用灵矽主动语音，但缺少配置：${missing.join("、")}`);
  }
  return {
    webSocketUrl: env.LINX_DEVICE_WS_URL ?? "wss://xrobo-io.qiniuapi.com/v1/ws/",
    token: required.token!,
    deviceId: required.deviceId!,
    clientId: required.clientId!,
    agentId: env.LINX_AGENT_ID || undefined,
    voiceId: env.LINX_VOICE_ID || undefined,
    timeoutMs: positiveInteger(env.LINX_VOICE_TIMEOUT_MS, 30_000),
  };
}

export function loadConfig(env: NodeJS.ProcessEnv = process.env): AppConfig {
  return {
    port: positiveInteger(env.PORT, 3000),
    databasePath: path.resolve(env.DATABASE_PATH ?? "./data/calendar.sqlite"),
    timeZone: env.APP_TIME_ZONE ?? "Asia/Shanghai",
    mcpSharedSecret: env.MCP_SHARED_SECRET ?? "dev-only-change-me",
    linxApiKey: env.LINX_API_KEY || undefined,
    demoMode: (env.DEMO_MODE ?? "true").toLowerCase() === "true",
    schedulerIntervalMs: positiveInteger(env.SCHEDULER_INTERVAL_MS, 1000),
    linxVoice: loadLinxVoiceConfig(env),
  };
}
