import type { Transport } from "@modelcontextprotocol/sdk/shared/transport.js";
import {
  JSONRPCMessageSchema,
  type JSONRPCMessage,
  type MessageExtraInfo,
} from "@modelcontextprotocol/sdk/types.js";

interface LinxProxyTokenResponse {
  code: number;
  msg: string;
  data: null | {
    wss_endpoint: string;
    expires_at: string;
  };
}

export interface LinxProxyCredentials {
  webSocketUrl: string;
  token: string;
  expiresAt: string;
}

export async function fetchLinxProxyCredentials(apiKey: string): Promise<LinxProxyCredentials> {
  const response = await fetch("https://xrobo.qiniu.com/v1/mcpproxy/token", {
    headers: { Authorization: `Bearer ${apiKey}` },
  });
  const body = await response.json() as LinxProxyTokenResponse;
  if (!response.ok || body.code !== 0 || !body.data) {
    throw new Error(`Failed to obtain Linx MCP Proxy token: ${body.msg || response.status}`);
  }

  const webSocketUrl = new URL(body.data.wss_endpoint);
  const token = webSocketUrl.searchParams.get("token");
  if (!token) throw new Error("Linx MCP Proxy response did not include a token");

  return {
    webSocketUrl: webSocketUrl.toString(),
    token,
    expiresAt: body.data.expires_at,
  };
}

async function messageDataToText(data: unknown): Promise<string> {
  if (typeof data === "string") return data;
  if (data instanceof Blob) return data.text();
  if (data instanceof ArrayBuffer) return new TextDecoder().decode(data);
  if (ArrayBuffer.isView(data)) {
    return new TextDecoder().decode(new Uint8Array(data.buffer, data.byteOffset, data.byteLength));
  }
  throw new Error("Unsupported Linx MCP Proxy WebSocket message type");
}

export class LinxMcpProxyTransport implements Transport {
  onclose?: () => void;
  onerror?: (error: Error) => void;
  onmessage?: <T extends JSONRPCMessage>(message: T, extra?: MessageExtraInfo) => void;

  private socket?: WebSocket;
  private started = false;
  private closed = false;

  constructor(private readonly webSocketUrl: string) {}

  async start(): Promise<void> {
    if (this.started) throw new Error("Linx MCP Proxy transport already started");
    this.started = true;

    await new Promise<void>((resolve, reject) => {
      const socket = new WebSocket(this.webSocketUrl);
      this.socket = socket;
      let opened = false;

      socket.onopen = () => {
        opened = true;
        resolve();
      };
      socket.onerror = () => {
        const error = new Error("Linx MCP Proxy WebSocket connection failed");
        this.onerror?.(error);
        if (!opened) reject(error);
      };
      socket.onclose = () => {
        if (!this.closed) this.onclose?.();
      };
      socket.onmessage = (event) => {
        void messageDataToText(event.data)
          .then((text) => JSONRPCMessageSchema.parse(JSON.parse(text)))
          .then((message) => this.onmessage?.(message))
          .catch((error: unknown) => {
            this.onerror?.(error instanceof Error ? error : new Error(String(error)));
          });
      };
    });
  }

  async send(message: JSONRPCMessage): Promise<void> {
    if (!this.socket || this.socket.readyState !== WebSocket.OPEN) {
      throw new Error("Linx MCP Proxy WebSocket is not connected");
    }
    this.socket.send(JSON.stringify(message));
  }

  async close(): Promise<void> {
    if (this.closed) return;
    this.closed = true;
    this.socket?.close();
    this.onclose?.();
  }
}
