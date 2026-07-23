import { once } from "node:events";
import type { AddressInfo } from "node:net";
import { afterEach, describe, expect, it, vi } from "vitest";
import { WebSocketServer } from "ws";
import {
  buildWavFile,
  LinxMacVoiceClient,
  type PcmAudioPlayer,
} from "../src/clients/linx-mac-voice-client.js";

const servers: WebSocketServer[] = [];

afterEach(async () => {
  while (servers.length) {
    const server = servers.pop()!;
    for (const client of server.clients) client.terminate();
    await new Promise<void>((resolve) => server.close(() => resolve()));
  }
});

describe("LinxMacVoiceClient", () => {
  it("injects text into a device session and plays returned PCM", async () => {
    const server = new WebSocketServer({ host: "127.0.0.1", port: 0 });
    servers.push(server);
    await once(server, "listening");
    const address = server.address() as AddressInfo;
    const inputPcm = Buffer.from([0x00, 0x00, 0x20, 0x00, 0xe0, 0xff]);
    let listenMessage: Record<string, unknown> | null = null;
    let requestHeaders: Record<string, string | string[] | undefined> = {};

    server.on("connection", (socket, request) => {
      requestHeaders = request.headers;
      socket.on("message", (data, isBinary) => {
        if (isBinary) return;
        const message = JSON.parse(data.toString()) as Record<string, unknown>;
        if (message.type === "hello") {
          socket.send(JSON.stringify({
            type: "hello",
            transport: "websocket",
            session_id: "test-session",
            audio_params: { format: "pcm", sample_rate: 16000, channels: 1 },
          }));
          return;
        }
        if (message.type === "listen") {
          listenMessage = message;
          socket.send(JSON.stringify({ type: "tts", state: "start" }));
          socket.send(JSON.stringify({
            type: "tts",
            state: "sentence_start",
            text: "提醒：现在该写日报了。",
          }));
          socket.send(inputPcm, { binary: true });
          socket.send(JSON.stringify({ type: "tts", state: "stop", is_aborted: false }));
        }
      });
    });

    const play = vi.fn<PcmAudioPlayer["play"]>(async () => undefined);
    const client = new LinxMacVoiceClient(
      {
        webSocketUrl: `ws://127.0.0.1:${address.port}`,
        token: "device-token",
        deviceId: "AA:BB:CC:DD:EE:FF",
        clientId: "client-uuid",
        agentId: "agent-id",
        timeoutMs: 2000,
      },
      { play },
    );

    try {
      const result = await client.speak("请播报提醒");
      expect(requestHeaders.authorization).toBe("Bearer device-token");
      expect(requestHeaders["device-id"]).toBe("AA:BB:CC:DD:EE:FF");
      expect(requestHeaders["client-id"]).toBe("client-uuid");
      expect(requestHeaders["x-agent-id"]).toBe("agent-id");
      expect(listenMessage).toMatchObject({
        session_id: "test-session",
        type: "listen",
        state: "detect",
        text: "请播报提醒",
      });
      expect(play).toHaveBeenCalledWith(inputPcm, { sampleRate: 16000, channels: 1 });
      expect(result).toEqual({
        spokenText: "提醒：现在该写日报了。",
        audioBytes: inputPcm.length,
        format: "pcm",
      });
    } finally {
      await client.close();
    }
  });

  it("builds a valid 16-bit PCM WAV container", () => {
    const pcm = Buffer.from([0x00, 0x00, 0xff, 0x7f]);
    const wav = buildWavFile(pcm, 16000, 1);
    expect(wav.toString("ascii", 0, 4)).toBe("RIFF");
    expect(wav.toString("ascii", 8, 12)).toBe("WAVE");
    expect(wav.readUInt32LE(24)).toBe(16000);
    expect(wav.readUInt16LE(34)).toBe(16);
    expect(wav.readUInt32LE(40)).toBe(pcm.length);
    expect(wav.subarray(44)).toEqual(pcm);
  });

  it("streams PCM packets before the TTS response finishes", async () => {
    const server = new WebSocketServer({ host: "127.0.0.1", port: 0 });
    servers.push(server);
    await once(server, "listening");
    const address = server.address() as AddressInfo;
    const firstPacket = Buffer.from([0x00, 0x00, 0x20, 0x00]);
    const secondPacket = Buffer.from([0xe0, 0xff, 0x00, 0x00]);
    const events: string[] = [];

    server.on("connection", (socket) => {
      socket.on("message", (data, isBinary) => {
        if (isBinary) return;
        const message = JSON.parse(data.toString()) as Record<string, unknown>;
        if (message.type === "hello") {
          socket.send(JSON.stringify({
            type: "hello",
            transport: "websocket",
            audio_params: { format: "pcm", sample_rate: 16000, channels: 1 },
          }));
          return;
        }
        if (message.type === "listen") {
          socket.send(JSON.stringify({ type: "tts", state: "start" }));
          socket.send(firstPacket, { binary: true });
          socket.send(JSON.stringify({ type: "tts", state: "sentence_start", text: "先显示这句话。" }));
          setTimeout(() => {
            events.push("server-stop");
            socket.send(secondPacket, { binary: true });
            socket.send(JSON.stringify({ type: "tts", state: "stop", is_aborted: false }));
          }, 20);
        }
      });
    });

    const play = vi.fn<PcmAudioPlayer["play"]>(async () => undefined);
    const client = new LinxMacVoiceClient(
      {
        webSocketUrl: `ws://127.0.0.1:${address.port}`,
        token: "device-token",
        deviceId: "AA:BB:CC:DD:EE:FF",
        clientId: "client-uuid",
        timeoutMs: 2000,
      },
      {
        play,
        async startStream(parameters) {
          events.push(`stream-start:${parameters.sampleRate}:${parameters.channels}`);
          return {
            async write(packet) {
              events.push(`write:${packet.toString("hex")}`);
            },
            async finish() {
              events.push("stream-finish");
            },
            abort() {
              events.push("stream-abort");
            },
          };
        },
      },
    );

    try {
      const result = await client.speak("测试流式播放", {
        onSpokenText(text) {
          events.push(`text:${text}`);
        },
      });
      expect(events).toEqual([
        "stream-start:16000:1",
        "text:先显示这句话。",
        `write:${firstPacket.toString("hex")}`,
        "server-stop",
        `write:${secondPacket.toString("hex")}`,
        "stream-finish",
      ]);
      expect(play).not.toHaveBeenCalled();
      expect(result.audioBytes).toBe(firstPacket.length + secondPacket.length);
      expect(result.spokenText).toBe("先显示这句话。");
    } finally {
      await client.close();
    }
  });

  it("supports a hello response without the optional session id", async () => {
    const server = new WebSocketServer({ host: "127.0.0.1", port: 0 });
    servers.push(server);
    await once(server, "listening");
    const address = server.address() as AddressInfo;
    let listenMessage: Record<string, unknown> | null = null;

    server.on("connection", (socket) => {
      socket.on("message", (data, isBinary) => {
        if (isBinary) return;
        const message = JSON.parse(data.toString()) as Record<string, unknown>;
        if (message.type === "hello") {
          socket.send(JSON.stringify({
            type: "hello",
            transport: "websocket",
            audio_params: { format: "pcm", sample_rate: 16000, channels: 1 },
          }));
          return;
        }
        listenMessage = message;
        socket.send(JSON.stringify({ type: "tts", state: "start" }));
        socket.send(Buffer.from([0x00, 0x00]), { binary: true });
        socket.send(JSON.stringify({ type: "tts", state: "stop", is_aborted: false }));
      });
    });

    const client = new LinxMacVoiceClient(
      {
        webSocketUrl: `ws://127.0.0.1:${address.port}`,
        token: "device-token",
        deviceId: "AA:BB:CC:DD:EE:FF",
        clientId: "client-uuid",
        timeoutMs: 2000,
      },
      { play: async () => undefined },
    );

    try {
      await client.speak("测试无会话编号");
      expect(listenMessage).toMatchObject({
        type: "listen",
        state: "detect",
        text: "测试无会话编号",
      });
      expect(listenMessage).not.toHaveProperty("session_id");
    } finally {
      await client.close();
    }
  });
});
