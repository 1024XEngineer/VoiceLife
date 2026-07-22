import "dotenv/config";
import { readFile, rename, writeFile } from "node:fs/promises";
import path from "node:path";
import { parse } from "dotenv";
import {
  createDeviceIdentity,
  LINX_DEMO_DEVICE_TOKEN,
  requestLinxDeviceActivation,
  upsertDotEnvValues,
} from "../clients/linx-device-activation.js";

const envPath = path.resolve(".env");
const examplePath = path.resolve(".env.example");

async function readStartingEnv(): Promise<string> {
  try {
    return await readFile(envPath, "utf8");
  } catch (error) {
    if ((error as NodeJS.ErrnoException).code !== "ENOENT") throw error;
    return readFile(examplePath, "utf8");
  }
}

async function saveEnv(content: string): Promise<void> {
  const temporaryPath = `${envPath}.linx-activate.tmp`;
  await writeFile(temporaryPath, content, { encoding: "utf8", mode: 0o600 });
  await rename(temporaryPath, envPath);
}

const initialEnv = await readStartingEnv();
const parsed = parse(initialEnv);
const identity = createDeviceIdentity({
  deviceId: parsed.LINX_DEVICE_ID,
  clientId: parsed.LINX_DEVICE_CLIENT_ID,
});

await saveEnv(upsertDotEnvValues(initialEnv, {
  LINX_DEVICE_ID: identity.deviceId,
  LINX_DEVICE_CLIENT_ID: identity.clientId,
}));

console.log(`Mac 虚拟设备 ID：${identity.deviceId}`);
console.log(`客户端 ID：${identity.clientId}`);
console.log("正在向灵矽 OTA 网关申请设备配置……");

const activation = await requestLinxDeviceActivation(identity, {
  otaUrl: process.env.LINX_DEVICE_OTA_URL || undefined,
});
const existingToken = parsed.LINX_DEVICE_WS_TOKEN?.trim();
const deviceToken = activation.webSocketToken ?? existingToken ?? LINX_DEMO_DEVICE_TOKEN;

const latestEnv = await readFile(envPath, "utf8");
await saveEnv(upsertDotEnvValues(latestEnv, {
  LINX_PROACTIVE_VOICE: "true",
  LINX_DEVICE_WS_URL: activation.webSocketUrl,
  LINX_DEVICE_WS_TOKEN: deviceToken,
  LINX_DEVICE_ID: identity.deviceId,
  LINX_DEVICE_CLIENT_ID: identity.clientId,
}));

console.log("设备连接配置已保存到本机 .env（Token 未输出）。");
if (!activation.webSocketToken && !existingToken) {
  console.log("OTA 响应未包含 Token，已采用灵矽公开客户端示例的兼容值 test-token。");
}

if (activation.activationCode) {
  console.log("");
  console.log(`设备激活码：${activation.activationCode}`);
  if (activation.activationMessage) console.log(activation.activationMessage);
  console.log("");
  console.log("请在灵矽智控台打开目标日程 Agent → 设备管理 → 新增设备，输入上面的激活码。");
  console.log("绑定完成后再次运行 npm run linx:voice:activate，然后运行 npm run linx:voice:test。");
} else {
  console.log("OTA 没有返回激活码；设备可能已经绑定。现在可运行 npm run linx:voice:test。");
}
