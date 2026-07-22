import { randomBytes, randomUUID } from "node:crypto";

export const DEFAULT_LINX_OTA_URL = "https://xrobo.qiniuapi.com/v1/ota/";
export const DEFAULT_LINX_DEVICE_WS_URL = "wss://xrobo-io.qiniuapi.com/v1/ws/";
export const LINX_DEMO_DEVICE_TOKEN = "test-token";

export interface LinxDeviceIdentity {
  deviceId: string;
  clientId: string;
}

export interface LinxDeviceActivationResult {
  activationCode?: string;
  activationMessage?: string;
  webSocketUrl: string;
  webSocketToken?: string;
  raw: Record<string, unknown>;
}

export interface FetchResponseLike {
  ok: boolean;
  status: number;
  text(): Promise<string>;
}

export type FetchLike = (
  input: string,
  init: { method: string; headers: Record<string, string>; body: string },
) => Promise<FetchResponseLike>;

function stringValue(value: unknown): string | undefined {
  return typeof value === "string" && value.trim() ? value.trim() : undefined;
}

function objectValue(value: unknown): Record<string, unknown> {
  return value && typeof value === "object" && !Array.isArray(value)
    ? value as Record<string, unknown>
    : {};
}

export function generatePseudoMacAddress(): string {
  const bytes = randomBytes(6);
  bytes[0] = (bytes[0]! | 0x02) & 0xfe;
  return [...bytes].map((byte) => byte.toString(16).padStart(2, "0")).join(":").toUpperCase();
}

export function createDeviceIdentity(existing: Partial<LinxDeviceIdentity> = {}): LinxDeviceIdentity {
  return {
    deviceId: existing.deviceId?.trim() || generatePseudoMacAddress(),
    clientId: existing.clientId?.trim() || randomUUID(),
  };
}

export function buildLinxOtaRequest(identity: LinxDeviceIdentity): Record<string, unknown> {
  return {
    version: 0,
    uuid: identity.clientId,
    application: {
      name: "linx-mac-voice-client",
      version: "0.1.0",
      compile_time: new Date().toISOString(),
      idf_version: "macOS",
      elf_sha256: "0".repeat(64),
    },
    ota: { label: "prototype" },
    board: {
      type: "macos",
      name: "linx-mac-voice-client",
      ssid: "",
      rssi: 0,
      channel: 0,
      ip: "127.0.0.1",
      mac: identity.deviceId,
    },
    flash_size: 0,
    minimum_free_heap_size: 0,
    mac_address: identity.deviceId,
    chip_model_name: "macos",
    chip_info: { model: 0, cores: 0, revision: 0, features: 0 },
    partition_table: [],
  };
}

export function parseLinxOtaResponse(raw: Record<string, unknown>): LinxDeviceActivationResult {
  const activation = objectValue(raw.activation);
  const webSocket = objectValue(raw.websocket);
  return {
    activationCode: stringValue(activation.code) ?? (
      typeof activation.code === "number" ? String(activation.code) : undefined
    ),
    activationMessage: stringValue(activation.message),
    webSocketUrl: stringValue(webSocket.url) ?? DEFAULT_LINX_DEVICE_WS_URL,
    webSocketToken: stringValue(webSocket.token)
      ?? stringValue(webSocket.access_token)
      ?? stringValue(raw.websocket_token),
    raw,
  };
}

export async function requestLinxDeviceActivation(
  identity: LinxDeviceIdentity,
  options: { otaUrl?: string; fetchImpl?: FetchLike } = {},
): Promise<LinxDeviceActivationResult> {
  const otaUrl = options.otaUrl ?? DEFAULT_LINX_OTA_URL;
  const fetchImpl = options.fetchImpl ?? (fetch as unknown as FetchLike);
  const response = await fetchImpl(otaUrl, {
    method: "POST",
    headers: {
      "Activation-Version": "1",
      "Device-Id": identity.deviceId,
      "Client-Id": identity.clientId,
      "User-Agent": "linx-voice-calendar-prototype/0.1.0",
      "Accept-Language": "zh-CN",
      "Content-Type": "application/json",
    },
    body: JSON.stringify(buildLinxOtaRequest(identity)),
  });
  const responseText = await response.text();
  let raw: Record<string, unknown>;
  try {
    raw = JSON.parse(responseText) as Record<string, unknown>;
  } catch {
    throw new Error(`灵矽 OTA 返回了非 JSON 响应（HTTP ${response.status}）`);
  }
  if (!response.ok || raw.error) {
    throw new Error(`灵矽 OTA 请求失败（HTTP ${response.status}）：${stringValue(raw.error) ?? responseText}`);
  }
  return parseLinxOtaResponse(raw);
}

function escapeRegExp(value: string): string {
  return value.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
}

export function upsertDotEnvValues(
  current: string,
  values: Record<string, string>,
): string {
  let output = current;
  for (const [key, value] of Object.entries(values)) {
    const line = `${key}=${JSON.stringify(value)}`;
    const pattern = new RegExp(`^${escapeRegExp(key)}\\s*=.*$`, "m");
    if (pattern.test(output)) output = output.replace(pattern, line);
    else output = `${output.replace(/\s*$/, "")}\n${line}\n`;
  }
  return output.endsWith("\n") ? output : `${output}\n`;
}
