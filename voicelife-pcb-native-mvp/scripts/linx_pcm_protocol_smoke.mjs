#!/usr/bin/env node

import { execFile } from "node:child_process";
import { createHash } from "node:crypto";
import { mkdtemp, readFile, rm } from "node:fs/promises";
import { tmpdir } from "node:os";
import path from "node:path";
import { createRequire } from "node:module";
import { promisify } from "node:util";
import { protocolToolResult, tools } from "./linx_pcm_protocol_tools.mjs";

const require = createRequire(new URL("../../XE6-15-pr62-fresh/package.json", import.meta.url));
const { WebSocket } = require("ws");
const execFileAsync = promisify(execFile);

const requiredEnv = ["LINX_DEVICE_ID", "LINX_DEVICE_CLIENT_ID", "LINX_AGENT_ID"];
for (const name of requiredEnv) {
  if (!process.env[name]?.trim()) throw new Error(`${name} is required`);
}

const config = {
  otaUrl: process.env.LINX_DEVICE_OTA_URL?.trim() || "https://xrobo.qiniuapi.com/v1/ota/",
  deviceId: process.env.LINX_DEVICE_ID.trim(),
  clientId: process.env.LINX_DEVICE_CLIENT_ID.trim(),
  agentId: process.env.LINX_AGENT_ID.trim(),
  phrase: process.env.VOICE_TEST_PHRASE?.trim() || "明天上午九点提醒我开会",
  voice: process.env.VOICE_TEST_VOICE?.trim() || "Tingting",
  timeoutMs: Number(process.env.VOICE_TEST_TIMEOUT_MS || 150_000),
};

function otaBody() {
  return {
    version: 0,
    uuid: config.clientId,
    application: {
      name: "voicelife-pcm-protocol-smoke",
      version: "0.1.0",
      compile_time: new Date().toISOString(),
      idf_version: "macOS",
      elf_sha256: "0".repeat(64),
    },
    ota: { label: "protocol-smoke" },
    board: {
      type: "macos",
      name: "voicelife-pcm-protocol-smoke",
      ssid: "",
      rssi: 0,
      channel: 0,
      ip: "127.0.0.1",
      mac: config.deviceId,
    },
    flash_size: 0,
    minimum_free_heap_size: 0,
    mac_address: config.deviceId,
    chip_model_name: "macos",
    chip_info: { model: 0, cores: 0, revision: 0, features: 0 },
    partition_table: [],
  };
}

async function getConnection() {
  const response = await fetch(config.otaUrl, {
    method: "POST",
    headers: {
      "Activation-Version": "1",
      "Device-Id": config.deviceId,
      "Client-Id": config.clientId,
      "User-Agent": "voicelife-pcm-protocol-smoke/0.1.0",
      "Accept-Language": "zh-CN",
      "Content-Type": "application/json",
    },
    body: JSON.stringify(otaBody()),
  });
  const body = await response.json();
  const webSocketUrl = body.websocket?.url;
  const token = body.websocket?.token || body.websocket?.access_token || body.websocket_token;
  if (!response.ok || !webSocketUrl || !token) {
    throw new Error(`Linx OTA did not return a usable WebSocket configuration (HTTP ${response.status})`);
  }
  return { webSocketUrl, token };
}

function readWavPcm(wav) {
  if (wav.toString("ascii", 0, 4) !== "RIFF" || wav.toString("ascii", 8, 12) !== "WAVE") {
    throw new Error("say did not produce a RIFF/WAVE file");
  }
  let format = null;
  let pcm = null;
  for (let offset = 12; offset + 8 <= wav.length;) {
    const type = wav.toString("ascii", offset, offset + 4);
    const size = wav.readUInt32LE(offset + 4);
    const start = offset + 8;
    if (type === "fmt ") {
      format = {
        encoding: wav.readUInt16LE(start),
        channels: wav.readUInt16LE(start + 2),
        sampleRate: wav.readUInt32LE(start + 4),
        bits: wav.readUInt16LE(start + 14),
      };
    } else if (type === "data") {
      pcm = wav.subarray(start, start + size);
    }
    offset = start + size + (size % 2);
  }
  if (!format || format.encoding !== 1 || format.channels !== 1 || format.sampleRate !== 16000 || format.bits !== 16 || !pcm) {
    throw new Error(`unexpected WAV format: ${JSON.stringify(format)}`);
  }
  return pcm;
}

async function generatePcm() {
  const directory = await mkdtemp(path.join(tmpdir(), "voicelife-pcm-smoke-"));
  const wavPath = path.join(directory, "input.wav");
  try {
    await execFileAsync("/usr/bin/say", [
      "-v", config.voice,
      "-r", "155",
      "--file-format=WAVE",
      "--data-format=LEI16@16000",
      "-o", wavPath,
      config.phrase,
    ]);
    const wav = await readFile(wavPath);
    return { pcm: readWavPcm(wav), wavSha256: createHash("sha256").update(wav).digest("hex") };
  } finally {
    await rm(directory, { recursive: true, force: true });
  }
}

function tomorrowAtNine() {
  const values = Object.fromEntries(new Intl.DateTimeFormat("en-CA", {
    timeZone: "Asia/Shanghai", year: "numeric", month: "2-digit", day: "2-digit",
  }).formatToParts(new Date()).filter((part) => part.type !== "literal").map((part) => [part.type, part.value]));
  const date = new Date(Date.UTC(Number(values.year), Number(values.month) - 1, Number(values.day) + 1));
  return `${date.toISOString().slice(0, 10)}T09:00:00+08:00`;
}

function sendMcp(socket, sessionId, id, result) {
  socket.send(JSON.stringify({
    ...(sessionId ? { session_id: sessionId } : {}),
    type: "mcp",
    payload: { jsonrpc: "2.0", id, result },
  }));
}

async function streamPcm(socket, sessionId, pcm) {
  const silence = Buffer.alloc(640 * 10);
  const audio = Buffer.concat([silence, pcm, silence]);
  socket.send(JSON.stringify({ session_id: sessionId, type: "listen", state: "start", mode: "realtime" }));
  for (let offset = 0; offset < audio.length; offset += 640) {
    socket.send(audio.subarray(offset, Math.min(offset + 640, audio.length)), { binary: true });
    await new Promise((resolve) => setTimeout(resolve, 20));
  }
  socket.send(JSON.stringify({ session_id: sessionId, type: "listen", state: "stop", mode: "realtime" }));
}

function helloMessage() {
  return JSON.stringify({
    type: "hello",
    version: 1,
    features: { mcp: true },
    transport: "websocket",
    audio_params: {
      format: "pcm", sample_rate: 16000, channels: 1, bit_depth: 16,
      endianness: "little", frame_duration: 20, frame_size: 320,
      sample_format: "signed_int16", play_buffer_duration: 1000,
    },
  });
}

function createObservation() {
  return {
    hello: false,
    mcpInitialize: false,
    mcpToolsList: false,
    sttTexts: [],
    toolCalls: [],
    ttsTexts: [],
    ttsAudioBytes: 0,
  };
}

function handleMcp({ socket, payload, observed, state, audio, finish }) {
  if (payload.method === "initialize") {
    observed.mcpInitialize = true;
    sendMcp(socket, state.sessionId, payload.id, {
      protocolVersion: "2024-11-05",
      capabilities: { tools: {} },
      serverInfo: { name: "VoiceLife PCB protocol smoke", version: "0.1.0" },
    });
    return;
  }
  if (payload.method === "tools/list") {
    observed.mcpToolsList = true;
    sendMcp(socket, state.sessionId, payload.id, { tools, nextCursor: null });
    if (!state.audioStarted) {
      state.audioStarted = true;
      void streamPcm(socket, state.sessionId, audio.pcm).catch(finish);
    }
    return;
  }
  if (payload.method !== "tools/call") return;
  const call = {
    name: payload.params?.name,
    arguments: payload.params?.arguments || {},
  };
  observed.toolCalls.push(call);
  sendMcp(socket, state.sessionId, payload.id, {
    content: [{ type: "text", text: JSON.stringify(protocolToolResult(call)) }],
    isError: false,
  });
}

function waitForCompletion(socket, audio, observed) {
  const state = { sessionId: null, audioStarted: false, settled: false };
  return new Promise((resolve, reject) => {
    const timeout = setTimeout(() => finish(new Error("Linx PCM protocol smoke timed out")), config.timeoutMs);
    const finish = (error) => {
      if (state.settled) return;
      state.settled = true;
      clearTimeout(timeout);
      if (error) reject(error);
      else resolve(observed);
    };

    socket.on("open", () => socket.send(helloMessage()));
    socket.on("error", finish);
    socket.on("close", () => {
      if (!observed.ttsAudioBytes) finish(new Error("Linx WebSocket closed before TTS completed"));
    });
    socket.on("message", (data, isBinary) => {
      try {
        if (isBinary) {
          observed.ttsAudioBytes += data.length;
          return;
        }
        const message = JSON.parse(data.toString("utf8"));
        if (message.type === "hello") {
          observed.hello = true;
          state.sessionId = message.session_id;
          return;
        }
        if (message.type === "stt" && typeof message.text === "string") observed.sttTexts.push(message.text);
        if (message.type === "tts" && message.state === "sentence_start") observed.ttsTexts.push(message.text);
        if (message.type === "tts" && message.state === "stop" && observed.ttsAudioBytes) finish();
        if (message.type === "mcp") {
          handleMcp({ socket, payload: message.payload || {}, observed, state, audio, finish });
        }
      } catch (error) {
        finish(error);
      }
    });
  });
}

function buildManifest(result, audio) {
  const create = result.toolCalls.find((call) => call.name === "calendar_create");
  const expectedStart = tomorrowAtNine();
  const expectedSpeech = create ? protocolToolResult(create).speech : null;
  const spokenText = result.ttsTexts.join("");
  const assertions = {
    hello: result.hello,
    mcpInitialize: result.mcpInitialize,
    mcpToolsList: result.mcpToolsList,
    sttContainsMeeting: result.sttTexts.some((text) => text.includes("提醒") && text.includes("开会")),
    calendarCreateCalled: Boolean(create),
    pointKind: create?.arguments.kind === "point",
    exactTomorrowAtNine: create?.arguments.startsAt === expectedStart,
    ttsExactlySpeech: Boolean(expectedSpeech) && spokenText === expectedSpeech,
    ttsAudioPresent: result.ttsAudioBytes > 0,
  };
  return {
    result: Object.values(assertions).every(Boolean) ? "pass" : "fail",
    capturedAt: new Date().toISOString(),
    phrase: config.phrase,
    voice: config.voice,
    input: { format: "pcm_s16le", sampleRate: 16000, channels: 1, bytes: audio.pcm.length, wavSha256: audio.wavSha256 },
    expectedStart,
    expectedSpeech,
    spokenText,
    observed: result,
    assertions,
    credentials: { otaTokenReceived: true, tokenPersisted: false, tokenLogged: false },
    limitations: [
      "MCP tool responses are deterministic protocol-test responses; this run does not write the physical PCB calendar.",
      "Physical PCB storage and reminder delivery are verified separately by C++ host tests and hardware auto-broadcast evidence.",
    ],
  };
}

async function run() {
  const [{ webSocketUrl, token }, audio] = await Promise.all([getConnection(), generatePcm()]);
  const observed = createObservation();
  const socket = new WebSocket(webSocketUrl, {
    headers: {
      Authorization: `Bearer ${token}`,
      "Device-Id": config.deviceId,
      "Client-Id": config.clientId,
      "Protocol-Version": "1",
      "X-Agent-ID": config.agentId,
    },
  });
  try {
    const result = await waitForCompletion(socket, audio, observed);
    const manifest = buildManifest(result, audio);
    console.log(JSON.stringify(manifest, null, 2));
    if (manifest.result !== "pass") process.exitCode = 1;
  } finally {
    socket.close();
  }
}

await run();
