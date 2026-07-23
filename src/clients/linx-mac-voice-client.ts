import { execFile, spawn } from "node:child_process";
import { createHash } from "node:crypto";
import { chmod, mkdir, mkdtemp, readFile, rename, rm, stat, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { promisify } from "node:util";
import OpusScript from "opusscript";
import { WebSocket, type RawData } from "ws";

const execFileAsync = promisify(execFile);
const VALID_OPUS_SAMPLE_RATES = new Set([8000, 12000, 16000, 24000, 48000]);

export interface LinxMacVoiceClientConfig {
  webSocketUrl: string;
  token: string;
  deviceId: string;
  clientId: string;
  agentId?: string;
  voiceId?: string;
  timeoutMs: number;
  requestedFormat?: "pcm" | "opus";
}

export interface AudioParameters {
  format: "pcm" | "opus";
  sampleRate: number;
  channels: number;
}

export interface VoicePlaybackResult {
  spokenText: string | null;
  audioBytes: number;
  format: "pcm" | "opus";
}

export interface SpeakOptions {
  onSpokenText?: (text: string) => void | Promise<void>;
}

export interface PcmAudioPlayer {
  play(pcm: Buffer, parameters: Omit<AudioParameters, "format">): Promise<void>;
  startStream?(parameters: Omit<AudioParameters, "format">): Promise<PcmAudioStream>;
}

export interface PcmAudioStream {
  write(pcm: Buffer): Promise<void>;
  finish(): Promise<void>;
  abort(): void;
}

interface PendingSpeech {
  packets: Buffer[];
  preTextPackets: Buffer[];
  audioBytes: number;
  stream: PcmAudioStream | null;
  streamWrites: Promise<void>;
  playbackGate: Promise<void>;
  textSeen: boolean;
  onSpokenText?: SpeakOptions["onSpokenText"];
  spokenText: string | null;
  started: boolean;
  resolve: (result: VoicePlaybackResult) => void;
  reject: (error: Error) => void;
  timeout: NodeJS.Timeout;
}

function rawDataToBuffer(data: RawData): Buffer {
  if (Buffer.isBuffer(data)) return data;
  if (Array.isArray(data)) return Buffer.concat(data);
  return Buffer.from(data);
}

function errorMessage(error: unknown): string {
  return error instanceof Error ? error.message : String(error);
}

export function buildWavFile(
  pcm: Buffer,
  sampleRate: number,
  channels: number,
  bitsPerSample = 16,
): Buffer {
  const header = Buffer.alloc(44);
  const blockAlign = (channels * bitsPerSample) / 8;
  const byteRate = sampleRate * blockAlign;

  header.write("RIFF", 0);
  header.writeUInt32LE(36 + pcm.length, 4);
  header.write("WAVE", 8);
  header.write("fmt ", 12);
  header.writeUInt32LE(16, 16);
  header.writeUInt16LE(1, 20);
  header.writeUInt16LE(channels, 22);
  header.writeUInt32LE(sampleRate, 24);
  header.writeUInt32LE(byteRate, 28);
  header.writeUInt16LE(blockAlign, 32);
  header.writeUInt16LE(bitsPerSample, 34);
  header.write("data", 36);
  header.writeUInt32LE(pcm.length, 40);
  return Buffer.concat([header, pcm]);
}

let macOsStreamingHelperPromise: Promise<string> | null = null;

async function ensureMacOsStreamingHelper(): Promise<string> {
  if (process.platform !== "darwin") {
    throw new Error("macOS 流式语音播放器只能在 macOS 上使用");
  }
  if (macOsStreamingHelperPromise) return macOsStreamingHelperPromise;

  macOsStreamingHelperPromise = (async () => {
    const sourcePath = fileURLToPath(
      new URL("../../native/macos-pcm-stream-player.c", import.meta.url),
    );
    const outputDirectory = path.resolve(path.dirname(sourcePath), "../local/bin");
    const source = await readFile(sourcePath);
    const sourceVersion = createHash("sha256").update(source).digest("hex").slice(0, 12);
    const outputPath = path.join(
      outputDirectory,
      `voicelife-pcm-stream-player-${sourceVersion}`,
    );
    await mkdir(outputDirectory, { recursive: true });

    const outputInfo = await stat(outputPath).catch(() => null);
    if (outputInfo?.isFile()) return outputPath;

    const temporaryPath = `${outputPath}.${process.pid}.tmp`;
    await execFileAsync("/usr/bin/clang", [
      sourcePath,
      "-O2",
      "-o",
      temporaryPath,
      "-framework",
      "AudioToolbox",
      "-framework",
      "CoreFoundation",
    ]);
    await chmod(temporaryPath, 0o755);
    await rename(temporaryPath, outputPath);
    return outputPath;
  })().catch((error) => {
    macOsStreamingHelperPromise = null;
    throw error;
  });
  void macOsStreamingHelperPromise.catch(() => undefined);

  return macOsStreamingHelperPromise;
}

class MacOsPcmStream implements PcmAudioStream {
  private readonly child;
  private readonly completion: Promise<void>;
  private stderr = "";
  private closed = false;

  public constructor(
    executablePath: string,
    parameters: Omit<AudioParameters, "format">,
  ) {
    this.child = spawn(executablePath, [String(parameters.sampleRate), String(parameters.channels)], {
      stdio: ["pipe", "ignore", "pipe"],
    });
    this.child.stderr.setEncoding("utf8");
    this.child.stderr.on("data", (chunk: string) => {
      if (this.stderr.length < 8_000) this.stderr += chunk;
    });
    this.child.stdin.on("error", () => undefined);
    this.completion = new Promise<void>((resolve, reject) => {
      this.child.once("error", reject);
      this.child.once("close", (code, signal) => {
        if (code === 0) {
          resolve();
          return;
        }
        const detail = this.stderr.trim()
          || `退出码 ${code ?? "无"}${signal ? `，信号 ${signal}` : ""}`;
        reject(new Error(`流式音频播放器异常退出：${detail}`));
      });
    });
    void this.completion.catch(() => undefined);
  }

  write(pcm: Buffer): Promise<void> {
    if (this.closed || this.child.stdin.destroyed) {
      return Promise.reject(new Error("流式音频播放器已经关闭"));
    }
    return new Promise<void>((resolve, reject) => {
      this.child.stdin.write(pcm, (error) => error ? reject(error) : resolve());
    });
  }

  async finish(): Promise<void> {
    if (!this.closed) {
      this.closed = true;
      this.child.stdin.end();
    }
    await this.completion;
  }

  abort(): void {
    if (this.closed) return;
    this.closed = true;
    this.child.stdin.destroy();
    this.child.kill("SIGTERM");
  }
}

export class MacOsWavPlayer implements PcmAudioPlayer {
  private readonly streamingHelper = process.platform === "darwin"
    ? ensureMacOsStreamingHelper()
    : null;

  async startStream(
    parameters: Omit<AudioParameters, "format">,
  ): Promise<PcmAudioStream> {
    if (!this.streamingHelper) {
      throw new Error("macOS 流式语音播放器只能在 macOS 上使用");
    }
    const executablePath = await this.streamingHelper;
    return new MacOsPcmStream(executablePath, parameters);
  }

  async play(
    pcm: Buffer,
    parameters: Omit<AudioParameters, "format">,
  ): Promise<void> {
    if (process.platform !== "darwin") {
      throw new Error("macOS 主动语音客户端只能在 macOS 上使用");
    }

    const directory = await mkdtemp(path.join(tmpdir(), "linx-calendar-voice-"));
    const wavPath = path.join(directory, "reminder.wav");
    try {
      await writeFile(
        wavPath,
        buildWavFile(pcm, parameters.sampleRate, parameters.channels),
      );
      await execFileAsync("/usr/bin/afplay", [wavPath]);
    } finally {
      await rm(directory, { recursive: true, force: true });
    }
  }
}

export class LinxMacVoiceClient {
  private socket: WebSocket | null = null;
  private sessionId: string | null = null;
  private handshakeComplete = false;
  private audioParameters: AudioParameters;
  private connectPromise: Promise<void> | null = null;
  private connectResolve: (() => void) | null = null;
  private connectReject: ((error: Error) => void) | null = null;
  private connectTimeout: NodeJS.Timeout | null = null;
  private pendingSpeech: PendingSpeech | null = null;
  private speechQueue: Promise<unknown> = Promise.resolve();

  public constructor(
    private readonly config: LinxMacVoiceClientConfig,
    private readonly audioPlayer: PcmAudioPlayer = new MacOsWavPlayer(),
  ) {
    this.audioParameters = {
      format: config.requestedFormat ?? "pcm",
      sampleRate: 16000,
      channels: 1,
    };
  }

  async connect(): Promise<void> {
    if (this.socket?.readyState === WebSocket.OPEN && this.handshakeComplete) return;
    if (this.connectPromise) return this.connectPromise;

    this.connectPromise = new Promise<void>((resolve, reject) => {
      this.connectResolve = resolve;
      this.connectReject = reject;
    });

    const headers: Record<string, string> = {
      Authorization: `Bearer ${this.config.token}`,
      "Device-Id": this.config.deviceId,
      "Client-Id": this.config.clientId,
      "Protocol-Version": "1",
    };
    if (this.config.agentId) headers["X-Agent-ID"] = this.config.agentId;

    const socket = new WebSocket(this.config.webSocketUrl, { headers });
    this.socket = socket;
    this.connectTimeout = setTimeout(() => {
      this.failConnection(new Error("连接灵矽设备 WebSocket 超时"));
      socket.terminate();
    }, this.config.timeoutMs);

    socket.on("open", () => {
      socket.send(JSON.stringify({
        type: "hello",
        version: 1,
        transport: "websocket",
        audio_params: {
          format: this.audioParameters.format,
          sample_rate: this.audioParameters.sampleRate,
          channels: this.audioParameters.channels,
          bit_depth: 16,
          endianness: "little",
          frame_duration: 20,
          frame_size: 320,
          sample_format: "signed_int16",
          play_buffer_duration: 1000,
        },
      }));
    });
    socket.on("message", (data, isBinary) => {
      if (isBinary) this.handleAudio(rawDataToBuffer(data));
      else this.handleControlMessage(rawDataToBuffer(data).toString("utf8"));
    });
    socket.on("error", (error) => {
      if (!this.handshakeComplete) this.failConnection(error);
      this.failSpeech(error);
    });
    socket.on("close", () => {
      this.socket = null;
      this.sessionId = null;
      this.handshakeComplete = false;
      this.failConnection(new Error("灵矽设备 WebSocket 在握手前关闭"));
      this.failSpeech(new Error("灵矽设备 WebSocket 已断开"));
    });

    try {
      await this.connectPromise;
    } finally {
      this.connectPromise = null;
      this.clearConnectWaiters();
    }
  }

  speak(text: string, options: SpeakOptions = {}): Promise<VoicePlaybackResult> {
    const trimmed = text.trim();
    if (!trimmed) return Promise.reject(new Error("播报文本不能为空"));
    const task = this.speechQueue.then(() => this.performSpeak(trimmed, options));
    this.speechQueue = task.catch(() => undefined);
    return task;
  }

  async close(): Promise<void> {
    const socket = this.socket;
    this.socket = null;
    this.sessionId = null;
    this.handshakeComplete = false;
    this.failSpeech(new Error("灵矽语音客户端正在关闭"));
    if (!socket || socket.readyState === WebSocket.CLOSED) return;

    await new Promise<void>((resolve) => {
      const timeout = setTimeout(() => {
        socket.terminate();
        resolve();
      }, 1000);
      socket.once("close", () => {
        clearTimeout(timeout);
        resolve();
      });
      socket.close(1000, "client shutdown");
    });
  }

  private async performSpeak(text: string, options: SpeakOptions): Promise<VoicePlaybackResult> {
    await this.connect();
    if (!this.socket || this.socket.readyState !== WebSocket.OPEN || !this.handshakeComplete) {
      throw new Error("灵矽设备 WebSocket 尚未就绪");
    }

    const stream = this.audioParameters.format === "pcm" && this.audioPlayer.startStream
      ? await this.audioPlayer.startStream({
          sampleRate: this.audioParameters.sampleRate,
          channels: this.audioParameters.channels,
        })
      : null;

    return new Promise<VoicePlaybackResult>((resolve, reject) => {
      const timeout = setTimeout(() => {
        this.failSpeech(new Error("等待灵矽 TTS 音频超时"));
      }, this.config.timeoutMs);
      this.pendingSpeech = {
        packets: [],
        preTextPackets: [],
        audioBytes: 0,
        stream,
        streamWrites: Promise.resolve(),
        playbackGate: Promise.resolve(),
        textSeen: false,
        onSpokenText: options.onSpokenText,
        spokenText: null,
        started: false,
        resolve,
        reject,
        timeout,
      };

      const agentParams = this.config.voiceId
        ? { voice: { id: this.config.voiceId } }
        : undefined;
      this.socket!.send(JSON.stringify({
        ...(this.sessionId ? { session_id: this.sessionId } : {}),
        type: "listen",
        state: "detect",
        text,
        ...(agentParams ? { agent_params: agentParams } : {}),
      }));
    });
  }

  private handleControlMessage(raw: string): void {
    let message: Record<string, unknown>;
    try {
      message = JSON.parse(raw) as Record<string, unknown>;
    } catch {
      return;
    }

    if (message.type === "hello") {
      const sessionId = typeof message.session_id === "string" ? message.session_id : null;
      this.sessionId = sessionId;
      this.handshakeComplete = true;
      this.applyAudioParameters(message.audio_params);
      this.connectResolve?.();
      return;
    }

    if (message.type === "error") {
      this.failSpeech(new Error(`灵矽返回错误：${JSON.stringify(message)}`));
      return;
    }

    if (message.type !== "tts" || !this.pendingSpeech) return;
    if (message.state === "start") {
      this.pendingSpeech.started = true;
      this.pendingSpeech.packets = [];
      return;
    }
    if (message.state === "sentence_start" && typeof message.text === "string") {
      const pending = this.pendingSpeech;
      pending.spokenText = pending.spokenText
        ? `${pending.spokenText}${message.text}`
        : message.text;
      pending.textSeen = true;
      pending.playbackGate = pending.playbackGate.then(async () => {
        await pending.onSpokenText?.(message.text as string);
      });
      for (const packet of pending.preTextPackets.splice(0)) {
        this.queueStreamPacket(pending, packet);
      }
      return;
    }
    if (message.state === "stop") {
      if (message.is_aborted === true) {
        this.failSpeech(new Error("灵矽 TTS 播放被中断"));
        return;
      }
      void this.finishSpeech();
    }
  }

  private handleAudio(packet: Buffer): void {
    if (!this.pendingSpeech?.started) return;
    const pending = this.pendingSpeech;
    const copiedPacket = Buffer.from(packet);
    pending.audioBytes += copiedPacket.length;
    if (!pending.stream) {
      pending.packets.push(copiedPacket);
      return;
    }
    if (!pending.textSeen) {
      pending.preTextPackets.push(copiedPacket);
      return;
    }
    this.queueStreamPacket(pending, copiedPacket);
  }

  private queueStreamPacket(pending: PendingSpeech, packet: Buffer): void {
    const write = pending.streamWrites
      .then(() => pending.playbackGate)
      .then(() => pending.stream!.write(packet));
    pending.streamWrites = write;
    void write.catch((error) => {
      if (this.pendingSpeech === pending) {
        this.failSpeech(new Error(`流式播放灵矽 TTS 失败：${errorMessage(error)}`));
      }
    });
  }

  private applyAudioParameters(value: unknown): void {
    if (!value || typeof value !== "object") return;
    const parameters = value as Record<string, unknown>;
    const format = parameters.format === "pcm" || parameters.format === "opus"
      ? parameters.format
      : this.audioParameters.format;
    const sampleRate = Number(parameters.sample_rate) || this.audioParameters.sampleRate;
    const channels = Number(parameters.channels) || this.audioParameters.channels;
    this.audioParameters = { format, sampleRate, channels };
  }

  private async finishSpeech(): Promise<void> {
    const pending = this.pendingSpeech;
    if (!pending) return;
    this.pendingSpeech = null;
    clearTimeout(pending.timeout);

    try {
      if (pending.audioBytes === 0) throw new Error("灵矽 TTS 未返回音频数据");
      if (pending.stream && pending.preTextPackets.length) {
        for (const packet of pending.preTextPackets.splice(0)) {
          this.queueStreamPacket(pending, packet);
        }
      }
      await pending.streamWrites;
      if (pending.stream) {
        await pending.stream.finish();
      } else {
        const pcm = this.decodeAudio(pending.packets);
        await this.audioPlayer.play(pcm, {
          sampleRate: this.audioParameters.sampleRate,
          channels: this.audioParameters.channels,
        });
      }
      pending.resolve({
        spokenText: pending.spokenText,
        audioBytes: pending.audioBytes,
        format: this.audioParameters.format,
      });
    } catch (error) {
      pending.stream?.abort();
      pending.reject(new Error(`播放灵矽 TTS 失败：${errorMessage(error)}`));
    }
  }

  private decodeAudio(packets: Buffer[]): Buffer {
    if (this.audioParameters.format === "pcm") return Buffer.concat(packets);
    if (!VALID_OPUS_SAMPLE_RATES.has(this.audioParameters.sampleRate)) {
      throw new Error(`不支持的 Opus 采样率：${this.audioParameters.sampleRate}`);
    }
    const decoder = new OpusScript(
      this.audioParameters.sampleRate as 8000 | 12000 | 16000 | 24000 | 48000,
      this.audioParameters.channels,
      OpusScript.Application.AUDIO,
    );
    try {
      return Buffer.concat(packets.map((packet) => decoder.decode(packet)));
    } finally {
      decoder.delete();
    }
  }

  private failConnection(error: Error): void {
    this.connectReject?.(error);
    this.clearConnectWaiters();
  }

  private clearConnectWaiters(): void {
    if (this.connectTimeout) clearTimeout(this.connectTimeout);
    this.connectTimeout = null;
    this.connectResolve = null;
    this.connectReject = null;
  }

  private failSpeech(error: Error): void {
    const pending = this.pendingSpeech;
    if (!pending) return;
    this.pendingSpeech = null;
    clearTimeout(pending.timeout);
    pending.stream?.abort();
    pending.reject(error);
  }
}
