import type { Server } from "node:http";
import type { IncomingMessage } from "node:http";
import { WebSocket, WebSocketServer, type RawData } from "ws";
import {
  VoiceInteractionInterruptedError,
  type PcmInteractionOptions,
  type PcmInputInteraction,
} from "../clients/linx-mac-voice-client.js";
import type { CalendarDatabase } from "../storage/database.js";

interface VoiceWebSocketDependencies {
  db: Pick<CalendarDatabase, "listReceipts">;
  voiceClient?: {
    startPcmInteraction(options?: PcmInteractionOptions): Promise<PcmInputInteraction>;
  };
}

function rawDataToBuffer(data: RawData): Buffer {
  if (Buffer.isBuffer(data)) return data;
  if (Array.isArray(data)) return Buffer.concat(data);
  return Buffer.from(data);
}

function isLocalHost(request: IncomingMessage): boolean {
  const host = request.headers.host ?? "";
  const hostname = host.startsWith("[")
    ? host.slice(0, host.indexOf("]") + 1)
    : host.split(":")[0];
  const remoteAddress = request.socket.remoteAddress ?? "";
  const isLoopback = remoteAddress === "::1"
    || remoteAddress.startsWith("127.")
    || remoteAddress.startsWith("::ffff:127.");
  return isLoopback
    && (hostname === "localhost" || hostname === "127.0.0.1" || hostname === "[::1]");
}

function send(socket: WebSocket, event: Record<string, unknown>): void {
  if (socket.readyState === WebSocket.OPEN) socket.send(JSON.stringify(event));
}

function errorMessage(error: unknown): string {
  return error instanceof Error ? error.message : String(error);
}

export function attachVoiceWebSocketServer(
  server: Server,
  deps: VoiceWebSocketDependencies,
): WebSocketServer {
  const webSocketServer = new WebSocketServer({
    server,
    path: "/api/voice/stream",
    maxPayload: 64 * 1024,
  });

  webSocketServer.on("connection", (socket, request) => {
    if (!isLocalHost(request)) {
      socket.close(1008, "local access only");
      return;
    }

    let activeInteraction: PcmInputInteraction | null = null;
    let turn = 0;

    const interruptActiveTurn = () => {
      activeInteraction?.interrupt("wake_word_detected");
      activeInteraction = null;
    };

    const beginTurn = async () => {
      const voiceClient = deps.voiceClient;
      if (!voiceClient) {
        send(socket, { type: "error", error: "语音服务尚未连接，请先完成 Mac 语音设备绑定" });
        return;
      }

      turn += 1;
      const currentTurn = turn;
      interruptActiveTurn();
      const receiptIdsBefore = new Set(deps.db.listReceipts(200).map((receipt) => receipt.id));
      const sentReceiptIds = new Set<string>();

      try {
        const interaction = await voiceClient.startPcmInteraction({
          onTranscription(text) {
            if (turn === currentTurn) send(socket, { type: "stt", text });
          },
          async onSpokenText(text) {
            if (turn !== currentTurn) return;
            const queryReceipt = [...deps.db.listReceipts(200)]
              .reverse()
              .find((receipt) =>
                receipt.type === "calendar_query"
                && !receiptIdsBefore.has(receipt.id)
                && !sentReceiptIds.has(receipt.id));
            if (queryReceipt) sentReceiptIds.add(queryReceipt.id);
            send(socket, { type: "message", text, receipt: queryReceipt ?? null });
          },
        });
        if (turn !== currentTurn) {
          interaction.interrupt();
          return;
        }
        activeInteraction = interaction;
        send(socket, {
          type: "listening",
          audio: { format: "pcm", sampleRate: 16000, channels: 1, frameSamples: 320 },
        });

        void interaction.result.then((result) => {
          if (turn !== currentTurn) return;
          activeInteraction = null;
          send(socket, {
            type: "complete",
            audioBytes: result.audioBytes,
            format: result.format,
          });
        }).catch((error: unknown) => {
          if (turn !== currentTurn) return;
          activeInteraction = null;
          if (error instanceof VoiceInteractionInterruptedError) {
            send(socket, { type: "interrupted" });
            return;
          }
          send(socket, { type: "error", error: errorMessage(error) });
        });
      } catch (error) {
        if (turn === currentTurn) send(socket, { type: "error", error: errorMessage(error) });
      }
    };

    socket.on("message", (data, isBinary) => {
      if (isBinary) {
        if (!activeInteraction) return;
        try {
          activeInteraction.writePcm(rawDataToBuffer(data));
        } catch (error) {
          send(socket, { type: "error", error: errorMessage(error) });
        }
        return;
      }

      let message: Record<string, unknown>;
      try {
        message = JSON.parse(rawDataToBuffer(data).toString("utf8")) as Record<string, unknown>;
      } catch {
        send(socket, { type: "error", error: "无法解析语音控制消息" });
        return;
      }

      if (message.type === "start") {
        void beginTurn();
        return;
      }
      if (message.type === "stop") {
        activeInteraction?.stopInput();
        send(socket, { type: "processing" });
        return;
      }
      if (message.type === "interrupt") {
        turn += 1;
        interruptActiveTurn();
        send(socket, { type: "interrupted" });
      }
    });

    socket.on("close", () => {
      turn += 1;
      interruptActiveTurn();
    });
  });

  return webSocketServer;
}

export async function closeVoiceWebSocketServer(server: WebSocketServer): Promise<void> {
  for (const client of server.clients) client.terminate();
  await new Promise<void>((resolve, reject) => {
    server.close((error) => error ? reject(error) : resolve());
  });
}
