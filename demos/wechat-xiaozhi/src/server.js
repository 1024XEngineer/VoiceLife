import { createApp } from "./app.js";
import { loadConfig } from "./config.js";
import { loadDotEnv } from "./env.js";
import { VoiceLifeService } from "./service.js";
import { JsonStore } from "./store.js";
import { WechatApi } from "./wechat-api.js";
import { XiaozhiMcpBridge } from "./xiaozhi-mcp.js";

loadDotEnv();
const config = loadConfig();
const store = new JsonStore(config.dataFile);
const wechatApi = new WechatApi(config.wechat);
const service = new VoiceLifeService({ store, wechatApi });
const app = createApp({ config, service, wechatApi });
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

app.listen(config.port, () => {
  console.info(`VoiceLife demo: ${config.baseUrl}`);
  console.info(`WeChat callback: ${config.baseUrl}/wechat/callback`);
  console.info(`WeChat outbound mode: ${config.wechat.outboundMode}`);
  bridge.start();
});

function shutdown() {
  bridge.stop();
  app.close(() => process.exit(0));
}

process.on("SIGINT", shutdown);
process.on("SIGTERM", shutdown);
