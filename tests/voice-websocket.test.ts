import { once } from "node:events";
import { createServer } from "node:http";
import type { AddressInfo } from "node:net";
import { afterEach, describe, expect, it, vi } from "vitest";
import { WebSocket } from "ws";
import type {
  PcmInputInteraction,
  PcmInteractionOptions,
  VoicePlaybackResult,
} from "../src/clients/linx-mac-voice-client.js";
import {
  attachVoiceWebSocketServer,
  closeVoiceWebSocketServer,
} from "../src/http/voice-websocket.js";

const cleanups: Array<() => Promise<void>> = [];

afterEach(async () => {
  while (cleanups.length) await cleanups.pop()!();
});

describe("browser PCM voice WebSocket", () => {
  it("bridges browser PCM frames to Linx and streams STT and reply events back", async () => {
    const httpServer = createServer((_request, response) => response.end());
    let options: PcmInteractionOptions = {};
    let resolveResult!: (result: VoicePlaybackResult) => void;
    const result = new Promise<VoicePlaybackResult>((resolve) => {
      resolveResult = resolve;
    });
    const receivedPcm: Buffer[] = [];
    let stopped = false;
    const interaction: PcmInputInteraction = {
      result,
      writePcm(pcm) {
        receivedPcm.push(Buffer.from(pcm));
      },
      stopInput() {
        stopped = true;
        void options.onTranscription?.("明天十点客户拜访");
        void options.onSpokenText?.("好的，已记录。");
        resolveResult({ spokenText: "好的，已记录。", audioBytes: 640, format: "pcm" });
      },
      interrupt() {},
    };
    const voiceClient = {
      async startPcmInteraction(receivedOptions: PcmInteractionOptions) {
        options = receivedOptions;
        return interaction;
      },
    };
    const webSocketServer = attachVoiceWebSocketServer(httpServer, {
      db: { listReceipts: () => [] },
      voiceClient,
    });
    await new Promise<void>((resolve) => httpServer.listen(0, "127.0.0.1", resolve));
    cleanups.push(async () => {
      await closeVoiceWebSocketServer(webSocketServer);
      await new Promise<void>((resolve) => httpServer.close(() => resolve()));
    });

    const address = httpServer.address() as AddressInfo;
    const socket = new WebSocket(`ws://127.0.0.1:${address.port}/api/voice/stream`);
    const events: Array<Record<string, unknown>> = [];
    socket.on("message", (data) => events.push(JSON.parse(data.toString())));
    await once(socket, "open");

    socket.send(JSON.stringify({ type: "start" }));
    await vi.waitFor(() => expect(events.some((event) => event.type === "listening")).toBe(true));
    const pcm = Buffer.from([0x00, 0x00, 0x20, 0x00]);
    socket.send(pcm, { binary: true });
    socket.send(JSON.stringify({ type: "stop" }));

    await vi.waitFor(() => expect(events.some((event) => event.type === "complete")).toBe(true));
    expect(stopped).toBe(true);
    expect(receivedPcm).toEqual([pcm]);
    expect(events).toEqual(expect.arrayContaining([
      expect.objectContaining({ type: "stt", text: "明天十点客户拜访" }),
      expect.objectContaining({ type: "message", text: "好的，已记录。" }),
      expect.objectContaining({ type: "complete", audioBytes: 640, format: "pcm" }),
    ]));
    socket.close();
  });
});
