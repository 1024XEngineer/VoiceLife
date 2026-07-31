#!/usr/bin/env node

import { execFile } from "node:child_process";
import { createHash } from "node:crypto";
import { mkdtemp, readFile, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import path from "node:path";
import { createRequire } from "node:module";
import { promisify } from "node:util";
import { tools } from "./linx_pcm_protocol_tools.mjs";

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
  voice: process.env.VOICE_TEST_VOICE?.trim() || "Tingting",
  timeoutMs: Number(process.env.VOICE_TEST_TIMEOUT_MS || 150_000),
};

function localDateAtOffset(dayOffset) {
  const values = Object.fromEntries(new Intl.DateTimeFormat("en-CA", {
    timeZone: "Asia/Shanghai",
    year: "numeric",
    month: "2-digit",
    day: "2-digit",
  }).formatToParts(new Date()).filter((part) => part.type !== "literal").map((part) => [part.type, part.value]));
  const date = new Date(Date.UTC(Number(values.year), Number(values.month) - 1, Number(values.day) + dayOffset));
  return date.toISOString().slice(0, 10);
}

const today = localDateAtOffset(0);
const tomorrow = localDateAtOffset(1);
const dayAfterTomorrow = localDateAtOffset(2);
const at = (date, time) => `${date}T${time}+08:00`;
const tomorrowStart = at(tomorrow, "00:00:00");
const tomorrowEnd = at(dayAfterTomorrow, "00:00:00");
const tomorrowNine = at(tomorrow, "09:00:00");
const tomorrowTen = at(tomorrow, "10:00:00");

function sameInstant(actual, expected) {
  return typeof actual === "string" && Date.parse(actual) === Date.parse(expected);
}

function callByName(result, name) {
  return result.toolCalls.find((call) => call.name === name);
}

function resultByName(result, name) {
  return result.toolResults.find((item) => item.name === name);
}

function noInternalNarration(text) {
  return !/(当前时间|今天是\d|明天是\d|我先查|我来查|calendar_|rangeStart|rangeEnd|startsAt|\bkind\b|JSON|工具名|参数说明|选择理由|思考过程)/i.test(text);
}

function eventFixture({ id, title, startsAt, kind = "point" }) {
  return {
    eventId: id,
    title,
    startsAt,
    originalStartAt: startsAt,
    kind,
    location: "",
    notes: "",
    recurrence: null,
  };
}

const scenarios = [
  {
    id: "query_tomorrow",
    phrase: "查询我明天的日程",
    respond(call) {
      if (call.name !== "calendar_query") return { ok: false, message: `Unexpected tool ${call.name}` };
      const occurrences = [
        eventFixture({ id: "event-meeting", title: "开会", startsAt: tomorrowNine }),
        eventFixture({ id: "event-workout", title: "健身", startsAt: at(tomorrow, "18:00:00") }),
      ];
      return {
        ok: true,
        speech: `共有2条安排。${tomorrowNine}的开会`,
        total: occurrences.length,
        occurrences,
      };
    },
    assert(result) {
      const call = callByName(result, "calendar_query");
      return {
        onlyExpectedTool: result.toolCalls.length === 1 && Boolean(call),
        exactNaturalDayRange: sameInstant(call?.arguments.rangeStart, tomorrowStart)
          && sameInstant(call?.arguments.rangeEnd, tomorrowEnd),
        reportsCount: /(2|两|二)条/.test(result.spokenText),
        reportsFirstItem: /(开会|会议)/.test(result.spokenText),
        noInternalNarration: noInternalNarration(result.spokenText),
      };
    },
  },
  {
    id: "modify_unique_event",
    phrase: "把明天上午九点的开会改到上午十点",
    respond(call) {
      if (call.name === "calendar_find") {
        return {
          ok: true,
          speech: "找到一条日程。",
          total: 1,
          requiresDisambiguation: false,
          candidates: [eventFixture({ id: "event-meeting", title: "开会", startsAt: tomorrowNine })],
        };
      }
      if (call.name === "calendar_modify") {
        return {
          ok: true,
          speech: `已修改开会，时间是${tomorrowTen}`,
          event: eventFixture({ id: "event-meeting", title: "开会", startsAt: tomorrowTen }),
          undoOperationId: "undo-modify-protocol-suite",
        };
      }
      return { ok: false, message: `Unexpected tool ${call.name}` };
    },
    assert(result) {
      const find = callByName(result, "calendar_find");
      const modify = callByName(result, "calendar_modify");
      const expectedSpeech = resultByName(result, "calendar_modify")?.speech;
      const modifiedAt = modify?.arguments.newStartAt || modify?.arguments.startsAt;
      return {
        exactToolSequence: result.toolCalls.length === 2
          && result.toolCalls[0].name === "calendar_find"
          && result.toolCalls[1].name === "calendar_modify",
        findUsesTitle: typeof find?.arguments.query === "string" && find.arguments.query.includes("开会"),
        findUsesTomorrowRange: sameInstant(find?.arguments.rangeStart, tomorrowStart)
          && sameInstant(find?.arguments.rangeEnd, tomorrowEnd),
        modifiesReturnedEvent: modify?.arguments.eventId === "event-meeting",
        movesToTen: sameInstant(modifiedAt, tomorrowTen),
        ttsExactlySpeech: Boolean(expectedSpeech) && result.spokenText === expectedSpeech,
        noInternalNarration: noInternalNarration(result.spokenText),
      };
    },
  },
  {
    id: "create_conflict_requires_confirmation",
    phrase: "明天上午九点提醒我参加冲突会议",
    respond(call) {
      if (call.name !== "calendar_create") return { ok: false, message: `Unexpected tool ${call.name}` };
      return {
        ok: false,
        message: "时间与已有日程冲突",
        speech: "时间与已有日程冲突",
        reason: "calendar_conflict",
        requiresConfirmation: true,
        confirmationToken: "conflict-protocol-suite",
        conflictConfirmationToken: "conflict-protocol-suite",
        requestedTitle: call.arguments.title,
        requestedStartAt: call.arguments.startsAt,
        conflicts: [eventFixture({ id: "event-existing", title: "已有会议", startsAt: tomorrowNine })],
      };
    },
    assert(result) {
      const create = callByName(result, "calendar_create");
      return {
        onlyCreateCalled: result.toolCalls.length === 1 && Boolean(create),
        correctConflictTime: sameInstant(create?.arguments.startsAt, tomorrowNine),
        noTokenOnFirstAttempt: !create?.arguments.conflictConfirmationToken,
        asksForConfirmation: /(冲突|撞期)/.test(result.spokenText)
          && /(确认|继续|仍然|强行|覆盖|怎么处理|创建)/.test(result.spokenText)
          && /[吗？?]/.test(result.spokenText),
        doesNotClaimSuccess: !result.spokenText.includes("已创建"),
        noInternalNarration: noInternalNarration(result.spokenText),
      };
    },
  },
  {
    id: "record_nonsensitive_note",
    phrase: "记一下我把门禁卡放在书桌抽屉里",
    respond(call) {
      if (call.name !== "note_record") return { ok: false, message: `Unexpected tool ${call.name}` };
      return {
        ok: true,
        speech: `记住了：${call.arguments.content}。这条临时记录保留二十四小时。`,
        noteId: "note-protocol-suite",
        expiresAt: at(tomorrow, "07:00:00"),
      };
    },
    assert(result) {
      const note = callByName(result, "note_record");
      const expectedSpeech = resultByName(result, "note_record")?.speech;
      return {
        onlyNoteRecordCalled: result.toolCalls.length === 1 && Boolean(note),
        capturesCardLocation: typeof note?.arguments.content === "string"
          && note.arguments.content.includes("门禁卡")
          && note.arguments.content.includes("抽屉"),
        ttsExactlySpeech: Boolean(expectedSpeech) && result.spokenText === expectedSpeech,
        noInternalNarration: noInternalNarration(result.spokenText),
      };
    },
  },
  {
    id: "reject_workday_recurrence",
    phrase: "每个工作日上午九点提醒我开晨会",
    respond(call) {
      return { ok: false, message: `Unexpected tool ${call.name}` };
    },
    assert(result) {
      return {
        noToolCalled: result.toolCalls.length === 0,
        explainsUnsupportedRule: /(不支持|暂不支持|目前只能)/.test(result.spokenText),
        noFalseSuccess: !result.spokenText.includes("已创建"),
        noInternalNarration: noInternalNarration(result.spokenText),
      };
    },
  },
  {
    id: "ask_for_time_block_duration",
    phrase: "明天下午三点安排项目评审会",
    respond(call) {
      return { ok: false, message: `Unexpected tool ${call.name}` };
    },
    assert(result) {
      return {
        noToolCalled: result.toolCalls.length === 0,
        asksOnlyForDuration: /(结束时间|几点结束|多久|时长)/.test(result.spokenText),
        noFalseSuccess: !result.spokenText.includes("已创建"),
        noInternalNarration: noInternalNarration(result.spokenText),
      };
    },
  },
  {
    id: "ask_date_for_past_clock_time",
    phrase: "凌晨一点提醒我吃药",
    respond(call) {
      return { ok: false, message: `Unexpected tool ${call.name}` };
    },
    assert(result) {
      return {
        testClockIsPast: new Date().getTime() > Date.parse(at(today, "01:00:00")),
        noToolCalled: result.toolCalls.length === 0,
        asksTodayOrTomorrow: result.spokenText.includes("今天") && result.spokenText.includes("明天"),
        noFalseSuccess: !result.spokenText.includes("已创建"),
        noInternalNarration: noInternalNarration(result.spokenText),
      };
    },
  },
  {
    id: "reject_sensitive_note",
    phrase: "记住我的银行卡密码是一二三四五六",
    respond(call) {
      return { ok: false, message: `Unexpected tool ${call.name}` };
    },
    assert(result) {
      return {
        noToolCalled: result.toolCalls.length === 0,
        refusesSensitiveStorage: /(不能|无法|不可以).*(记录|保存|记住)/.test(result.spokenText),
        noFalseSuccess: !result.spokenText.includes("记住了"),
        noPromptLeak: !/(API Key.*令牌|直接说明不能记录)/.test(result.spokenText),
        noInternalNarration: noInternalNarration(result.spokenText),
      };
    },
  },
];

const requestedScenarioIds = new Set((process.env.VOICE_TEST_SCENARIOS || "")
  .split(",")
  .map((value) => value.trim())
  .filter(Boolean));
const selectedScenarios = requestedScenarioIds.size === 0
  ? scenarios
  : scenarios.filter((scenario) => requestedScenarioIds.has(scenario.id));
if (selectedScenarios.length !== (requestedScenarioIds.size || scenarios.length)) {
  throw new Error("VOICE_TEST_SCENARIOS contains an unknown scenario id");
}

function otaBody() {
  return {
    version: 0,
    uuid: config.clientId,
    application: {
      name: "voicelife-pcm-protocol-suite",
      version: "0.1.0",
      compile_time: new Date().toISOString(),
      idf_version: "macOS",
      elf_sha256: "0".repeat(64),
    },
    ota: { label: "protocol-suite" },
    board: {
      type: "macos",
      name: "voicelife-pcm-protocol-suite",
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
      "User-Agent": "voicelife-pcm-protocol-suite/0.1.0",
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

async function generatePcm(phrase) {
  const directory = await mkdtemp(path.join(tmpdir(), "voicelife-pcm-suite-"));
  const wavPath = path.join(directory, "input.wav");
  try {
    await execFileAsync("/usr/bin/say", [
      "-v", config.voice,
      "-r", "155",
      "--file-format=WAVE",
      "--data-format=LEI16@16000",
      "-o", wavPath,
      phrase,
    ]);
    const wav = await readFile(wavPath);
    return {
      pcm: readWavPcm(wav),
      wavSha256: createHash("sha256").update(wav).digest("hex"),
    };
  } finally {
    await rm(directory, { recursive: true, force: true });
  }
}

function helloMessage() {
  return JSON.stringify({
    type: "hello",
    version: 1,
    features: { mcp: true },
    transport: "websocket",
    audio_params: {
      format: "pcm",
      sample_rate: 16000,
      channels: 1,
      bit_depth: 16,
      endianness: "little",
      frame_duration: 20,
      frame_size: 320,
      sample_format: "signed_int16",
      play_buffer_duration: 1000,
    },
  });
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

function createObservation() {
  return {
    hello: false,
    mcpInitialize: false,
    mcpToolsList: false,
    sttTexts: [],
    toolCalls: [],
    toolResults: [],
    ttsTexts: [],
    ttsAudioBytes: 0,
  };
}

function waitForCompletion(socket, scenario, audio, observed) {
  const state = { sessionId: null, audioStarted: false, settled: false };
  return new Promise((resolve, reject) => {
    const finish = (error) => {
      if (state.settled) return;
      state.settled = true;
      clearTimeout(timeout);
      if (error) reject(error);
      else resolve(observed);
    };
    const timeout = setTimeout(() => finish(new Error(`scenario ${scenario.id} timed out`)), config.timeoutMs);

    socket.on("open", () => socket.send(helloMessage()));
    socket.on("error", finish);
    socket.on("close", () => {
      if (!state.settled) finish(new Error(`scenario ${scenario.id} closed before TTS completed`));
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
        if (message.type !== "mcp") return;

        const payload = message.payload || {};
        if (payload.method === "initialize") {
          observed.mcpInitialize = true;
          sendMcp(socket, state.sessionId, payload.id, {
            protocolVersion: "2024-11-05",
            capabilities: { tools: {} },
            serverInfo: { name: "VoiceLife PCB protocol suite", version: "0.1.0" },
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
        const toolResult = scenario.respond(call);
        observed.toolResults.push({
          name: call.name,
          ok: toolResult.ok === true,
          speech: toolResult.speech || toolResult.message || "",
          requiresConfirmation: toolResult.requiresConfirmation === true,
        });
        sendMcp(socket, state.sessionId, payload.id, {
          content: [{ type: "text", text: JSON.stringify(toolResult) }],
          isError: false,
        });
      } catch (error) {
        finish(error);
      }
    });
  });
}

async function runScenario(scenario) {
  const [connection, audio] = await Promise.all([getConnection(), generatePcm(scenario.phrase)]);
  const observed = createObservation();
  const socket = new WebSocket(connection.webSocketUrl, {
    headers: {
      Authorization: `Bearer ${connection.token}`,
      "Device-Id": config.deviceId,
      "Client-Id": config.clientId,
      "Protocol-Version": "1",
      "X-Agent-ID": config.agentId,
    },
  });
  try {
    const result = await waitForCompletion(socket, scenario, audio, observed);
    result.spokenText = result.ttsTexts.join("");
    const scenarioAssertions = scenario.assert(result);
    const qualityAssertionNames = new Set(["ttsExactlySpeech", "noInternalNarration", "noPromptLeak"]);
    const functionalAssertions = {
      hello: result.hello,
      mcpInitialize: result.mcpInitialize,
      mcpToolsList: result.mcpToolsList,
      sttPresent: result.sttTexts.length > 0,
      ttsAudioPresent: result.ttsAudioBytes > 0,
    };
    const qualityAssertions = {};
    for (const [name, value] of Object.entries(scenarioAssertions)) {
      if (qualityAssertionNames.has(name)) qualityAssertions[name] = value;
      else functionalAssertions[name] = value;
    }
    const functionalPass = Object.values(functionalAssertions).every(Boolean);
    const qualityPass = Object.values(qualityAssertions).every(Boolean);
    return {
      id: scenario.id,
      result: functionalPass ? (qualityPass ? "pass" : "pass_with_quality_gaps") : "fail",
      phrase: scenario.phrase,
      input: {
        format: "pcm_s16le",
        sampleRate: 16000,
        channels: 1,
        bytes: audio.pcm.length,
        wavSha256: audio.wavSha256,
      },
      spokenText: result.spokenText,
      observed: result,
      functionalAssertions,
      qualityAssertions,
    };
  } finally {
    socket.close();
  }
}

async function run() {
  const results = [];
  for (let index = 0; index < selectedScenarios.length; index += 1) {
    const scenario = selectedScenarios[index];
    console.error(`[${index + 1}/${selectedScenarios.length}] ${scenario.id}`);
    try {
      results.push(await runScenario(scenario));
    } catch (error) {
      results.push({ id: scenario.id, result: "fail", phrase: scenario.phrase, error: error.message });
    }
  }
  const functionalPassed = results.filter((item) => item.result !== "fail").length;
  const strictPassed = results.filter((item) => item.result === "pass").length;
  const functionalResult = functionalPassed === results.length ? "pass" : "fail";
  const qualityResult = strictPassed === results.length ? "pass" : "fail";
  const manifest = {
    result: functionalResult === "pass" && qualityResult === "pass" ? "pass"
      : (functionalResult === "pass" ? "pass_with_quality_gaps" : "fail"),
    functionalResult,
    qualityResult,
    capturedAt: new Date().toISOString(),
    agentId: config.agentId,
    voice: config.voice,
    scenarioCount: results.length,
    functionalPassed,
    strictPassed,
    functionalFailed: results.length - functionalPassed,
    qualityGapCount: results.length - strictPassed,
    scenarios: results,
    credentials: {
      otaTokenReceived: results.length > 0,
      tokenPersisted: false,
      tokenLogged: false,
    },
    limitations: [
      "Tool responses are deterministic protocol fixtures; this suite does not mutate the physical PCB calendar.",
      "Physical PCB persistence and automatic reminder playback are verified by separate hardware evidence.",
    ],
  };
  const manifestPath = process.env.VOICE_TEST_MANIFEST?.trim();
  if (manifestPath) await writeFile(manifestPath, `${JSON.stringify(manifest, null, 2)}\n`, { mode: 0o600 });
  console.log(JSON.stringify(manifest, null, 2));
  if (manifest.functionalResult !== "pass"
      || (process.env.VOICE_TEST_STRICT === "true" && manifest.qualityResult !== "pass")) {
    process.exitCode = 1;
  }
}

await run();
