import { readFile, rename, writeFile } from "node:fs/promises";
import path from "node:path";
import { parse } from "dotenv";
import { fetchLinxProxyCredentials } from "../adapters/linx-mcp-proxy.js";
import {
  createDeviceIdentity,
  LINX_DEMO_DEVICE_TOKEN,
  requestLinxDeviceActivation,
  upsertDotEnvValues,
} from "../clients/linx-device-activation.js";
import { LinxMacVoiceClient } from "../clients/linx-mac-voice-client.js";

export interface LinxSettingsStatus {
  apiKeyConfigured: boolean;
  agentId: string;
  voiceId: string;
  voiceEnabled: boolean;
  deviceId: string;
  clientId: string;
  deviceTokenConfigured: boolean;
  webSocketUrl: string;
}

export interface LinxSettingsInput {
  apiKey?: string;
  agentId?: string;
  voiceId?: string;
}

function stringSetting(value: unknown, name: string, maxLength = 500): string | undefined {
  if (value === undefined) return undefined;
  if (typeof value !== "string") throw new Error(`${name} 必须是字符串`);
  const trimmed = value.trim();
  if (trimmed.length > maxLength) throw new Error(`${name} 不能超过 ${maxLength} 个字符`);
  return trimmed;
}

export class LinxSettingsService {
  public constructor(
    private readonly envPath = path.resolve(".env"),
    private readonly examplePath = path.resolve(".env.example"),
  ) {}

  async getStatus(): Promise<LinxSettingsStatus> {
    const env = parse(await this.readEnv());
    return {
      apiKeyConfigured: Boolean(env.LINX_API_KEY?.trim()),
      agentId: env.LINX_AGENT_ID?.trim() ?? "",
      voiceId: env.LINX_VOICE_ID?.trim() ?? "",
      voiceEnabled: env.LINX_PROACTIVE_VOICE?.toLowerCase() === "true",
      deviceId: env.LINX_DEVICE_ID?.trim() ?? "",
      clientId: env.LINX_DEVICE_CLIENT_ID?.trim() ?? "",
      deviceTokenConfigured: Boolean(env.LINX_DEVICE_WS_TOKEN?.trim()),
      webSocketUrl: env.LINX_DEVICE_WS_URL?.trim()
        ?? "wss://xrobo-io.qiniuapi.com/v1/ws/",
    };
  }

  async save(input: LinxSettingsInput): Promise<LinxSettingsStatus> {
    const apiKey = stringSetting(input.apiKey, "API Key", 2_000);
    const agentId = stringSetting(input.agentId, "Agent ID");
    const voiceId = stringSetting(input.voiceId, "音色 ID");
    const values: Record<string, string> = {};
    if (apiKey) values.LINX_API_KEY = apiKey;
    if (agentId !== undefined) values.LINX_AGENT_ID = agentId;
    if (voiceId !== undefined) values.LINX_VOICE_ID = voiceId;
    if (Object.keys(values).length > 0) await this.updateEnv(values);
    return this.getStatus();
  }

  async createMcpToken(): Promise<{ token: string; expiresAt: string }> {
    const env = parse(await this.readEnv());
    const apiKey = env.LINX_API_KEY?.trim();
    if (!apiKey) throw new Error("请先保存灵矽 API Key");
    const credentials = await fetchLinxProxyCredentials(apiKey);
    return { token: credentials.token, expiresAt: credentials.expiresAt };
  }

  async activateDevice(): Promise<{
    activationCode?: string;
    activationMessage?: string;
    deviceId: string;
    clientId: string;
    voiceEnabled: boolean;
  }> {
    const initialText = await this.readEnv();
    const initialEnv = parse(initialText);
    const identity = createDeviceIdentity({
      deviceId: initialEnv.LINX_DEVICE_ID,
      clientId: initialEnv.LINX_DEVICE_CLIENT_ID,
    });
    await this.updateEnv({
      LINX_DEVICE_ID: identity.deviceId,
      LINX_DEVICE_CLIENT_ID: identity.clientId,
    });

    const activation = await requestLinxDeviceActivation(identity, {
      otaUrl: initialEnv.LINX_DEVICE_OTA_URL?.trim() || undefined,
    });
    const deviceToken = activation.webSocketToken
      ?? initialEnv.LINX_DEVICE_WS_TOKEN?.trim()
      ?? LINX_DEMO_DEVICE_TOKEN;
    await this.updateEnv({
      LINX_PROACTIVE_VOICE: "true",
      LINX_DEVICE_WS_URL: activation.webSocketUrl,
      LINX_DEVICE_WS_TOKEN: deviceToken,
      LINX_DEVICE_ID: identity.deviceId,
      LINX_DEVICE_CLIENT_ID: identity.clientId,
    });
    return {
      activationCode: activation.activationCode,
      activationMessage: activation.activationMessage,
      deviceId: identity.deviceId,
      clientId: identity.clientId,
      voiceEnabled: true,
    };
  }

  async testVoice(): Promise<{ reply: string | null; audioBytes: number; format: "pcm" | "opus" }> {
    const env = parse(await this.readEnv());
    const token = env.LINX_DEVICE_WS_TOKEN?.trim();
    const deviceId = env.LINX_DEVICE_ID?.trim();
    const clientId = env.LINX_DEVICE_CLIENT_ID?.trim();
    if (!token || !deviceId || !clientId) {
      throw new Error("请先完成 Mac 语音设备激活");
    }

    const client = new LinxMacVoiceClient({
      webSocketUrl: env.LINX_DEVICE_WS_URL?.trim()
        || "wss://xrobo-io.qiniuapi.com/v1/ws/",
      token,
      deviceId,
      clientId,
      agentId: env.LINX_AGENT_ID?.trim() || undefined,
      voiceId: env.LINX_VOICE_ID?.trim() || undefined,
      timeoutMs: Number.parseInt(env.LINX_VOICE_TIMEOUT_MS ?? "30000", 10) || 30_000,
    });
    try {
      const result = await client.speak("请只回复：语音连接测试成功");
      return { reply: result.spokenText, audioBytes: result.audioBytes, format: result.format };
    } finally {
      await client.close();
    }
  }

  private async readEnv(): Promise<string> {
    try {
      return await readFile(this.envPath, "utf8");
    } catch (error) {
      if ((error as NodeJS.ErrnoException).code !== "ENOENT") throw error;
      return readFile(this.examplePath, "utf8");
    }
  }

  private async updateEnv(values: Record<string, string>): Promise<void> {
    const content = upsertDotEnvValues(await this.readEnv(), values);
    const temporaryPath = `${this.envPath}.linx-settings.tmp`;
    await writeFile(temporaryPath, content, { encoding: "utf8", mode: 0o600 });
    await rename(temporaryPath, this.envPath);
  }
}
