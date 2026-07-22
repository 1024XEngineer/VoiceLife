import { LinxMacVoiceClient } from "../clients/linx-mac-voice-client.js";
import { loadConfig } from "../config.js";

const config = loadConfig();
if (!config.linxVoice) {
  throw new Error("请先在 .env 中设置 LINX_PROACTIVE_VOICE=true 并填写设备 WebSocket 配置");
}

const client = new LinxMacVoiceClient({
  webSocketUrl: config.linxVoice.webSocketUrl,
  token: config.linxVoice.token,
  deviceId: config.linxVoice.deviceId,
  clientId: config.linxVoice.clientId,
  agentId: config.linxVoice.agentId,
  voiceId: config.linxVoice.voiceId,
  timeoutMs: config.linxVoice.timeoutMs,
});

const text = process.argv.slice(2).join(" ").trim()
  || "请只回复：灵矽主动语音客户端连接成功。";

try {
  console.log("正在连接灵矽设备 WebSocket……");
  const result = await client.speak(text);
  console.log(`播放成功：${result.format}，接收 ${result.audioBytes} 字节音频`);
} finally {
  await client.close();
}
