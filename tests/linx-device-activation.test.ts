import { describe, expect, it } from "vitest";
import {
  buildLinxOtaRequest,
  createDeviceIdentity,
  parseLinxOtaResponse,
  requestLinxDeviceActivation,
  upsertDotEnvValues,
} from "../src/clients/linx-device-activation.js";

describe("Linx device activation", () => {
  it("keeps existing identity fields stable", () => {
    expect(createDeviceIdentity({
      deviceId: "02:11:22:33:44:55",
      clientId: "30ee0cbe-1ed6-43c8-b2d2-3fef3c1da553",
    })).toEqual({
      deviceId: "02:11:22:33:44:55",
      clientId: "30ee0cbe-1ed6-43c8-b2d2-3fef3c1da553",
    });
  });

  it("builds a consistent OTA identity payload", () => {
    const body = buildLinxOtaRequest({
      deviceId: "02:11:22:33:44:55",
      clientId: "client-id",
    });
    expect(body).toMatchObject({
      uuid: "client-id",
      mac_address: "02:11:22:33:44:55",
      board: { mac: "02:11:22:33:44:55" },
    });
  });

  it("parses activation and websocket credentials", () => {
    expect(parseLinxOtaResponse({
      activation: { code: 608303, message: "请绑定" },
      websocket: { url: "wss://example.test/ws", token: "secret" },
    })).toMatchObject({
      activationCode: "608303",
      activationMessage: "请绑定",
      webSocketUrl: "wss://example.test/ws",
      webSocketToken: "secret",
    });
  });

  it("sends required OTA headers", async () => {
    let headers: Record<string, string> = {};
    const result = await requestLinxDeviceActivation(
      { deviceId: "02:11:22:33:44:55", clientId: "client-id" },
      {
        fetchImpl: async (_input, init) => {
          headers = init.headers;
          return {
            ok: true,
            status: 200,
            text: async () => JSON.stringify({ websocket: { url: "wss://example.test/ws" } }),
          };
        },
      },
    );
    expect(headers).toMatchObject({
      "Activation-Version": "1",
      "Device-Id": "02:11:22:33:44:55",
      "Client-Id": "client-id",
    });
    expect(result.webSocketUrl).toBe("wss://example.test/ws");
  });

  it("updates dotenv values without removing other secrets", () => {
    const updated = upsertDotEnvValues(
      "LINX_API_KEY=keep-me\nLINX_PROACTIVE_VOICE=false\n",
      {
        LINX_PROACTIVE_VOICE: "true",
        LINX_DEVICE_ID: "02:11:22:33:44:55",
      },
    );
    expect(updated).toContain("LINX_API_KEY=keep-me");
    expect(updated).toContain('LINX_PROACTIVE_VOICE="true"');
    expect(updated).toContain('LINX_DEVICE_ID="02:11:22:33:44:55"');
  });
});
