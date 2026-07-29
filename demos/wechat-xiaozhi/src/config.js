import path from "node:path";
import process from "node:process";

function bool(value, fallback = false) {
  if (value === undefined) return fallback;
  return ["1", "true", "yes", "on"].includes(String(value).toLowerCase());
}

function number(value, fallback) {
  const parsed = Number(value);
  return Number.isFinite(parsed) ? parsed : fallback;
}

export function loadConfig(env = process.env, cwd = process.cwd()) {
  return {
    port: number(env.PORT, 8787),
    baseUrl: env.BASE_URL || "http://localhost:8787",
    dataFile: path.resolve(cwd, env.DATA_FILE || "./data/state.json"),
    deviceApiKey: env.DEVICE_API_KEY || "change-me",
    demoMode: bool(env.DEMO_MODE, true),
    runtime: env.IM_RUNTIME || "koishi-satori",
    wechat: {
      account: env.WECHAT_ACCOUNT || "",
      token: env.WECHAT_TOKEN || "voicelife-demo-token",
      appId: env.WECHAT_APP_ID || "",
      appSecret: env.WECHAT_APP_SECRET || "",
      aesKey: env.WECHAT_AES_KEY || "",
      outboundMode: env.WECHAT_OUTBOUND_MODE || "mock",
      templateId: env.WECHAT_TEMPLATE_ID || "",
      templateFields: {
        title: env.WECHAT_TEMPLATE_TITLE_FIELD || "thing1",
        time: env.WECHAT_TEMPLATE_TIME_FIELD || "time2",
        status: env.WECHAT_TEMPLATE_STATUS_FIELD || "thing3"
      },
      detailUrl: env.WECHAT_TEMPLATE_DETAIL_URL || "",
      actionTokenSecret: env.WECHAT_ACTION_TOKEN_SECRET || env.DEVICE_API_KEY || "change-me",
      actionTokenTtlSeconds: number(env.WECHAT_ACTION_TOKEN_TTL_SECONDS, 7 * 24 * 60 * 60)
    },
    koishi: {
      selfUrl: env.KOISHI_SELF_URL || env.BASE_URL || "http://localhost:8787",
      satoriPath: env.SATORI_PATH || "/satori",
      satoriToken: env.SATORI_TOKEN || env.DEVICE_API_KEY || "change-me"
    },
    xiaozhi: {
      endpoint: env.XIAOZHI_MCP_ENDPOINT || "",
      deviceId: env.XIAOZHI_DEVICE_ID || "xiaozhi-demo-01",
      debug: bool(env.XIAOZHI_MCP_DEBUG, false)
    }
  };
}
