import { createRequire } from "node:module";
import { loadConfig } from "./config.js";
import { loadDotEnv } from "./env.js";
import { createVoiceLifeKoishiPlugin } from "./koishi-plugin.js";
import { createImApplication } from "./im-application.js";
import { VoiceLifeService } from "./service.js";
import { JsonStore } from "./store.js";
import {
  createBindingServicePort,
  createReminderCommandPort
} from "./voicelife-ports.js";
import { WechatApi } from "./wechat-api.js";
import { XiaozhiMcpBridge } from "./xiaozhi-mcp.js";

// Koishi 4.18 的顶层 ESM Loader 与 Node.js 26 存在互操作问题。
// CommonJS 入口可正常提供完整 Context，并保持官方插件生命周期。
const require = createRequire(import.meta.url);
const { Context } = require("koishi");
const { HTTP } = require("@koishijs/plugin-http");
const { WechatOfficialBot } = require("@koishijs/plugin-adapter-wechat-official");
const server = require("@koishijs/plugin-server").default;
const satoriServer = require("@koishijs/plugin-server-satori").default;

loadDotEnv();
const config = loadConfig();
const store = new JsonStore(config.dataFile);
const wechatApi = new WechatApi(config.wechat);
const service = new VoiceLifeService({ store, wechatApi });
const imApplication = createImApplication({
  bindingService: createBindingServicePort(service),
  reminderCommandPort: createReminderCommandPort(service)
});
const app = new Context();
app.plugin(HTTP);
app.plugin(server, {
  host: "0.0.0.0",
  port: config.port,
  selfUrl: config.koishi.selfUrl
});
app.plugin(createVoiceLifeKoishiPlugin({
  config,
  service,
  imApplication,
  wechatApi
}));
if (config.wechat.account) {
  app.plugin(WechatOfficialBot, {
    account: config.wechat.account,
    appid: config.wechat.appId,
    secret: config.wechat.appSecret,
    token: config.wechat.token,
    aesKey: config.wechat.aesKey,
    customerService: false
  });
} else {
  console.warn("[koishi] 缺少 WECHAT_ACCOUNT，先保留旧回调；收到微信消息后可从日志取得 gh_... 原始 ID");
}
app.plugin(satoriServer, {
  path: config.koishi.satoriPath,
  token: config.koishi.satoriToken,
  webhooks: []
});
const bridge = new XiaozhiMcpBridge({
  endpoint: config.xiaozhi.endpoint,
  deviceId: config.xiaozhi.deviceId,
  service,
  debug: config.xiaozhi.debug
});

const scheduler = setInterval(() => {
  service.dispatchDue().catch((error) => console.error("[scheduler]", error));
}, 1000);
scheduler.unref();

await app.start();
console.info(`VoiceLife demo: http://localhost:${config.port}`);
console.info(`Koishi WeChat callback: ${config.koishi.selfUrl}/wechat-official`);
console.info(`Satori endpoint: ${config.koishi.selfUrl}${config.koishi.satoriPath}/v1`);
console.info(`WeChat outbound mode: ${config.wechat.outboundMode}`);
bridge.start();

async function shutdown() {
  bridge.stop();
  await app.stop();
  process.exit(0);
}

process.on("SIGINT", shutdown);
process.on("SIGTERM", shutdown);
