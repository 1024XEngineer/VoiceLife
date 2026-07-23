const talkButton = document.querySelector("#talk-button");
const buttonLabel = document.querySelector("#button-label");
const voiceTab = document.querySelector("#voice-tab");
const messagesTab = document.querySelector("#messages-tab");
const voicePanel = document.querySelector("#voice-panel");
const messagesPanel = document.querySelector("#messages-panel");
const messages = document.querySelector("#messages");
const empty = document.querySelector("#empty");
const connection = document.querySelector("#connection");
const unread = document.querySelector("#unread");
const conversationFeed = document.querySelector("#conversation-feed");
const scheduleCard = document.querySelector("#schedule-card");
const scheduleTitle = document.querySelector("#schedule-title");
const scheduleDate = document.querySelector("#schedule-date");
const scheduleCount = document.querySelector("#schedule-count");
const scheduleItems = document.querySelector("#schedule-items");

let holding = false;
let busy = false;
let activePointerId = null;
let resetTimer = null;
let voiceSocket = null;
let voiceSocketPromise = null;
let listeningWaiter = null;
let activeCapture = null;
let recordingSequence = 0;
let assistantBubble = null;
let actionResultShown = false;
let lastTranscription = "";
let receipts = [];
let activeTab = "voice";
let unreadCount = 0;
const configuredBubbleLimit = Number.parseInt(localStorage.getItem("voiceBubbleLimit") ?? "3", 10);
const bubbleLimit = [1, 3, 5].includes(configuredBubbleLimit) ? configuredBubbleLimit : 3;
const bubbleLifetimeMs = 12_000;
const bubbleTimers = new WeakMap();
let bubbleSequence = 0;

const receiptLabels = {
  calendar_created: "日程已创建",
  calendar_query: "查询结果",
  calendar_rescheduled: "本次日程已修改",
  calendar_modified: "日程已修改",
  calendar_skipped: "本次日程已跳过",
  calendar_paused: "周期日程已暂停",
  calendar_resumed: "周期日程已恢复",
  calendar_terminated: "周期日程已终止",
  calendar_deleted: "日程已删除",
  calendar_undone: "操作已撤销",
  note_recorded: "临时事项已记录",
  note_query: "临时记录查询",
  reminder_due: "提醒到达",
  reminder_weak_due: "提前提示",
  reminder_closed: "提醒已关闭",
  reminder_snoozed: "提醒已推迟",
};

const states = {
  idle: {
    button: "按住说话",
  },
  listening: {
    button: "松开发送",
  },
  processing: {
    button: "正在处理",
  },
  replying: {
    button: "按住打断",
  },
  success: {
    button: "继续说话",
  },
  error: {
    button: "重新说话",
  },
};

function setState(name, overrides = {}) {
  const state = { ...states[name], ...overrides };
  document.body.dataset.state = name;
  buttonLabel.textContent = state.button;
  talkButton.setAttribute("aria-label", state.button);
}

function scheduleIdle(delay = 2200) {
  clearTimeout(resetTimer);
  resetTimer = setTimeout(() => {
    if (!holding && !busy) setState("idle");
  }, delay);
}

function escapeHtml(value) {
  return String(value)
    .replaceAll("&", "&amp;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;")
    .replaceAll('"', "&quot;")
    .replaceAll("'", "&#039;");
}

function showConversation() {
  conversationFeed.hidden = false;
  scheduleCard.hidden = true;
}

function dismissConversationBubble(bubble) {
  if (!bubble?.isConnected || bubble.classList.contains("leaving")) return;
  clearTimeout(bubbleTimers.get(bubble));
  bubble.classList.add("leaving");
  setTimeout(() => bubble.remove(), 420);
}

function scheduleConversationBubbleDismissal(bubble) {
  clearTimeout(bubbleTimers.get(bubble));
  bubbleTimers.set(bubble, setTimeout(() => dismissConversationBubble(bubble), bubbleLifetimeMs));
}

function addConversationBubble(role, text) {
  if (!text) return null;
  showConversation();
  const bubble = document.createElement("p");
  bubble.className = `conversation-bubble ${role} entering`;
  bubble.dataset.bubbleId = String(++bubbleSequence);
  bubble.textContent = text;
  conversationFeed.append(bubble);
  requestAnimationFrame(() => bubble.classList.remove("entering"));
  scheduleConversationBubbleDismissal(bubble);

  const bubbles = [...conversationFeed.querySelectorAll(".conversation-bubble:not(.leaving)")];
  const excess = Math.max(0, bubbles.length - bubbleLimit);
  for (const oldBubble of bubbles.slice(0, excess)) {
    dismissConversationBubble(oldBubble);
  }
  return bubble;
}

function appendReplyText(current, next) {
  if (!current) return next;
  const needsSpace = /[a-z0-9]$/i.test(current) && /^[a-z0-9]/i.test(next);
  return `${current}${needsSpace ? " " : ""}${next}`;
}

function formatClock(value) {
  if (!value) return "—";
  return new Date(value).toLocaleTimeString("zh-CN", {
    hour: "2-digit",
    minute: "2-digit",
    hour12: false,
    timeZone: "Asia/Shanghai",
  });
}

function renderScheduleCard(receipt) {
  const occurrences = Array.isArray(receipt?.data?.occurrences)
    ? [...receipt.data.occurrences].sort((left, right) =>
      String(left.effectiveStartAt).localeCompare(String(right.effectiveStartAt)))
    : [];
  const rangeStart = receipt?.data?.rangeStart;
  const rangeDate = rangeStart ? new Date(rangeStart) : null;
  const todayLabel = new Date().toLocaleDateString("zh-CN", { timeZone: "Asia/Shanghai" });
  const rangeLabel = rangeDate?.toLocaleDateString("zh-CN", { timeZone: "Asia/Shanghai" });

  scheduleTitle.textContent = rangeLabel === todayLabel ? "今日日程" : "日程安排";
  scheduleDate.textContent = rangeDate
    ? rangeDate.toLocaleDateString("zh-CN", {
        month: "long",
        day: "numeric",
        weekday: "long",
        timeZone: "Asia/Shanghai",
      })
    : "查询结果";
  scheduleCount.textContent = `${occurrences.length} 项`;
  scheduleItems.innerHTML = occurrences.length
    ? occurrences.map((item) => `
        <article class="schedule-item">
          <div class="schedule-time start-time">
            <span>开始</span>
            <strong>${escapeHtml(formatClock(item.effectiveStartAt))}</strong>
          </div>
          <span class="schedule-rule" aria-hidden="true"></span>
          <div class="schedule-time end-time">
            <span>截止</span>
            <strong${item.effectiveEndAt ? "" : ' class="time-unset"'}>${escapeHtml(formatClock(item.effectiveEndAt))}</strong>
          </div>
          <div class="schedule-copy">
            <h3>${escapeHtml(item.title)}</h3>
            ${item.location ? `<p>${escapeHtml(item.location)}</p>` : ""}
          </div>
        </article>`).join("")
    : '<div class="schedule-empty">这个时间范围内没有日程</div>';
  conversationFeed.hidden = true;
  scheduleCard.hidden = false;
}

function setActiveTab(tab) {
  activeTab = tab;
  const showVoice = tab === "voice";
  voiceTab.classList.toggle("active", showVoice);
  messagesTab.classList.toggle("active", !showVoice);
  voiceTab.setAttribute("aria-selected", String(showVoice));
  messagesTab.setAttribute("aria-selected", String(!showVoice));
  voicePanel.hidden = !showVoice;
  messagesPanel.hidden = showVoice;

  if (!showVoice) {
    unreadCount = 0;
    unread.hidden = true;
    void loadReceipts();
  }
}

function activeReceiptIds() {
  const statesByReminder = new Map();
  for (const receipt of receipts) {
    if (!receipt.reminderId) continue;
    if (receipt.type === "reminder_due") statesByReminder.set(receipt.reminderId, receipt.id);
    if (receipt.type === "reminder_closed" || receipt.type === "reminder_snoozed") {
      statesByReminder.set(receipt.reminderId, null);
    }
  }
  return new Set([...statesByReminder.values()].filter(Boolean));
}

function renderReceipts() {
  const activeReminders = activeReceiptIds();
  const undoneOperationIds = new Set(
    receipts
      .filter((receipt) => receipt.type === "calendar_undone")
      .map((receipt) => receipt.data?.operationId)
      .filter(Boolean),
  );
  empty.hidden = receipts.length > 0;
  messages.innerHTML = [...receipts]
    .reverse()
    .map((receipt) => {
      const reminderActions = receipt.type === "reminder_due" && activeReminders.has(receipt.id)
        ? `<div class="message-actions">
            <button type="button" data-close="${escapeHtml(receipt.reminderId)}">知道了</button>
            <button type="button" data-snooze="${escapeHtml(receipt.reminderId)}">10 分钟后</button>
          </div>`
        : "";
      const undoOperationId = receipt.data?.undoOperationId;
      const undoAction = undoOperationId && !undoneOperationIds.has(undoOperationId)
        ? `<div class="message-actions">
            <button type="button" data-undo="${escapeHtml(undoOperationId)}">撤销操作</button>
          </div>`
        : "";
      const time = new Date(receipt.createdAt).toLocaleString("zh-CN", {
        month: "numeric",
        day: "numeric",
        hour: "2-digit",
        minute: "2-digit",
        hour12: false,
      });
      return `<article class="message ${escapeHtml(receipt.type)}">
        <div class="message-meta">
          <span>${escapeHtml(receiptLabels[receipt.type] ?? receipt.title)}</span>
          <time>${escapeHtml(time)}</time>
        </div>
        <p>${escapeHtml(receipt.body)}</p>
        ${receipt.data?.scheduleChanged === false ? '<span class="tag">日程未改变</span>' : ""}
        ${reminderActions}${undoAction}
      </article>`;
    })
    .join("");
}

async function request(url, options = {}) {
  const response = await fetch(url, {
    ...options,
    headers: { "Content-Type": "application/json", ...(options.headers ?? {}) },
  });
  const body = await response.json();
  if (!response.ok) throw new Error(body.error ?? "操作失败");
  return body;
}

function voiceSocketUrl() {
  const protocol = window.location.protocol === "https:" ? "wss:" : "ws:";
  return `${protocol}//${window.location.host}/api/voice/stream`;
}

function rejectListeningWaiter(error) {
  if (!listeningWaiter) return;
  clearTimeout(listeningWaiter.timeout);
  listeningWaiter.reject(error);
  listeningWaiter = null;
}

function handleVoiceEvent(event) {
  if (event.type === "listening") {
    if (listeningWaiter) {
      clearTimeout(listeningWaiter.timeout);
      listeningWaiter.resolve(event.audio);
      listeningWaiter = null;
    }
    return;
  }
  if (event.type === "processing") {
    if (!holding) setState("processing");
    return;
  }
  if (event.type === "stt") {
    const text = String(event.text ?? "").trim();
    if (text && text !== lastTranscription) {
      lastTranscription = text;
      addConversationBubble("user", text);
    }
    return;
  }
  if (event.type === "message") {
    setState("replying");
    if (event.receipt?.type === "calendar_query") {
      actionResultShown = true;
      renderScheduleCard(event.receipt);
    } else if (!actionResultShown && event.text) {
      if (!assistantBubble?.isConnected) {
        assistantBubble = addConversationBubble("assistant", event.text);
      } else {
        assistantBubble.textContent = appendReplyText(assistantBubble.textContent, event.text);
        scheduleConversationBubbleDismissal(assistantBubble);
      }
    }
    return;
  }
  if (event.type === "complete") {
    busy = false;
    setState("success");
    scheduleIdle(3000);
    return;
  }
  if (event.type === "interrupted") {
    if (!holding) {
      busy = false;
      setState("idle");
    }
    return;
  }
  if (event.type === "error") {
    const error = new Error(event.error ?? "语音交互失败");
    rejectListeningWaiter(error);
    busy = false;
    holding = false;
    setState("error");
    scheduleIdle(3200);
  }
}

function ensureVoiceSocket() {
  if (voiceSocket?.readyState === WebSocket.OPEN) return Promise.resolve(voiceSocket);
  if (voiceSocketPromise) return voiceSocketPromise;

  voiceSocketPromise = new Promise((resolve, reject) => {
    const socket = new WebSocket(voiceSocketUrl());
    socket.binaryType = "arraybuffer";
    voiceSocket = socket;
    socket.addEventListener("open", () => resolve(socket), { once: true });
    socket.addEventListener("message", (message) => {
      if (typeof message.data !== "string") return;
      try {
        handleVoiceEvent(JSON.parse(message.data));
      } catch {
        handleVoiceEvent({ type: "error", error: "收到无法解析的语音服务消息" });
      }
    });
    socket.addEventListener("error", () => reject(new Error("无法连接本地语音服务")), { once: true });
    socket.addEventListener("close", () => {
      if (voiceSocket === socket) voiceSocket = null;
      voiceSocketPromise = null;
      rejectListeningWaiter(new Error("本地语音连接已断开"));
      if (holding || busy) handleVoiceEvent({ type: "error", error: "本地语音连接已断开" });
    });
  }).finally(() => {
    voiceSocketPromise = null;
  });
  return voiceSocketPromise;
}

async function startServerTurn() {
  const socket = await ensureVoiceSocket();
  rejectListeningWaiter(new Error("新的语音输入已开始"));
  assistantBubble = null;
  actionResultShown = false;
  lastTranscription = "";
  return new Promise((resolve, reject) => {
    const timeout = setTimeout(() => {
      listeningWaiter = null;
      reject(new Error("等待灵矽进入监听状态超时"));
    }, 10_000);
    listeningWaiter = { resolve, reject, timeout };
    socket.send(JSON.stringify({ type: "start" }));
  });
}

function createPcmPacketizer(sourceSampleRate, onFrame) {
  const targetSampleRate = 16000;
  const frameSamples = 320;
  let frame = new Int16Array(frameSamples);
  let frameOffset = 0;
  let phase = 0;
  let accumulator = 0;
  let accumulatorSamples = 0;
  const pushSample = (sample) => {
    const clamped = Math.max(-1, Math.min(1, sample));
    frame[frameOffset] = clamped < 0
      ? Math.round(clamped * 0x8000)
      : Math.round(clamped * 0x7fff);
    frameOffset += 1;
    if (frameOffset !== frameSamples) return;
    onFrame(frame.buffer);
    frame = new Int16Array(frameSamples);
    frameOffset = 0;
  };
  return (samples) => {
    if (sourceSampleRate === targetSampleRate) {
      for (const sample of samples) pushSample(sample);
      return;
    }
    for (const sample of samples) {
      accumulator += sample;
      accumulatorSamples += 1;
      phase += targetSampleRate;
      if (phase >= sourceSampleRate) {
        pushSample(accumulator / accumulatorSamples);
        phase -= sourceSampleRate;
        accumulator = 0;
        accumulatorSamples = 0;
      }
    }
  };
}

async function createPcmCapture(onFrame) {
  if (!navigator.mediaDevices?.getUserMedia) {
    throw new Error("当前浏览器无法访问麦克风，请使用 localhost 或 HTTPS 打开页面");
  }
  const AudioContext = window.AudioContext ?? window.webkitAudioContext;
  if (!AudioContext) throw new Error("当前浏览器不支持实时 PCM 音频采集");
  const stream = await navigator.mediaDevices.getUserMedia({
    audio: {
      channelCount: 1,
      echoCancellation: true,
      noiseSuppression: true,
      autoGainControl: true,
    },
  });
  let context;
  try {
    context = new AudioContext({ sampleRate: 16000, latencyHint: "interactive" });
  } catch {
    context = new AudioContext();
  }
  try {
    const source = context.createMediaStreamSource(stream);
    const silentOutput = context.createGain();
    silentOutput.gain.value = 0;
    let processor;
    let detachProcessor;
    if (context.audioWorklet && window.AudioWorkletNode) {
      await context.audioWorklet.addModule("/pcm-capture-worklet.js");
      processor = new AudioWorkletNode(context, "pcm-capture", {
        numberOfInputs: 1,
        numberOfOutputs: 1,
        outputChannelCount: [1],
        processorOptions: { targetSampleRate: 16000, frameSamples: 320 },
      });
      processor.port.onmessage = (message) => onFrame(message.data);
      detachProcessor = () => {
        processor.port.onmessage = null;
      };
    } else if (context.createScriptProcessor) {
      processor = context.createScriptProcessor(1024, 1, 1);
      const packetize = createPcmPacketizer(context.sampleRate, onFrame);
      processor.onaudioprocess = (event) => packetize(event.inputBuffer.getChannelData(0));
      detachProcessor = () => {
        processor.onaudioprocess = null;
      };
    } else {
      throw new Error("当前浏览器不支持 AudioWorklet 或兼容 PCM 采集接口");
    }
    source.connect(processor);
    processor.connect(silentOutput);
    silentOutput.connect(context.destination);
    await context.resume();

    let stopped = false;
    return {
      async stop() {
        if (stopped) return;
        stopped = true;
        detachProcessor();
        source.disconnect();
        processor.disconnect();
        silentOutput.disconnect();
        for (const track of stream.getTracks()) track.stop();
        await context.close();
      },
    };
  } catch (error) {
    for (const track of stream.getTracks()) track.stop();
    await context.close();
    throw error;
  }
}

async function loadReceipts() {
  try {
    const data = await request("/api/receipts");
    receipts = data.receipts;
    renderReceipts();
    return receipts;
  } catch {
    connection.textContent = "连接失败";
    connection.classList.remove("online");
    return receipts;
  }
}

async function beginHold(event) {
  if (holding) return;
  clearTimeout(resetTimer);

  if (event instanceof PointerEvent) {
    if (!event.isPrimary || event.button !== 0) return;
    activePointerId = event.pointerId;
    talkButton.setPointerCapture?.(event.pointerId);
  }

  event.preventDefault();
  const shouldInterrupt = busy;
  const sequence = ++recordingSequence;
  holding = true;
  busy = true;
  navigator.vibrate?.(18);
  setState("listening");

  try {
    const socket = await ensureVoiceSocket();
    if (shouldInterrupt) socket.send(JSON.stringify({ type: "interrupt" }));
    const queuedFrames = [];
    let streaming = false;
    const capture = await createPcmCapture((frame) => {
      if (streaming && voiceSocket?.readyState === WebSocket.OPEN) {
        voiceSocket.send(frame);
      } else if (queuedFrames.length < 50) {
        queuedFrames.push(frame);
      }
    });
    if (sequence !== recordingSequence) {
      await capture.stop();
      return;
    }
    activeCapture = capture;
    await startServerTurn();
    streaming = true;
    for (const frame of queuedFrames) socket.send(frame);
    if (!holding) await finishCapture();
  } catch (error) {
    if (sequence !== recordingSequence) return;
    holding = false;
    busy = false;
    activeCapture?.stop();
    activeCapture = null;
    setState("error");
    console.error(error);
    scheduleIdle(3000);
  }
}

async function finishCapture() {
  const capture = activeCapture;
  activeCapture = null;
  await capture?.stop();
  if (voiceSocket?.readyState === WebSocket.OPEN) {
    voiceSocket.send(JSON.stringify({ type: "stop" }));
  }
}

function endHold(event) {
  if (!holding) return;
  if (event instanceof PointerEvent && activePointerId !== null && event.pointerId !== activePointerId) return;
  event.preventDefault();
  holding = false;
  activePointerId = null;
  setState("processing");
  navigator.vibrate?.(10);
  void finishCapture();
}

talkButton.addEventListener("pointerdown", beginHold);
talkButton.addEventListener("pointerup", endHold);
talkButton.addEventListener("pointercancel", endHold);
talkButton.addEventListener("lostpointercapture", endHold);
talkButton.addEventListener("contextmenu", (event) => event.preventDefault());

talkButton.addEventListener("keydown", (event) => {
  if ((event.key === " " || event.key === "Enter") && !event.repeat) beginHold(event);
});
talkButton.addEventListener("keyup", (event) => {
  if (event.key === " " || event.key === "Enter") endHold(event);
});

voiceTab.addEventListener("click", () => setActiveTab("voice"));
messagesTab.addEventListener("click", () => setActiveTab("messages"));

messages.addEventListener("click", async (event) => {
  const closeButton = event.target.closest("[data-close]");
  const snoozeButton = event.target.closest("[data-snooze]");
  const undoButton = event.target.closest("[data-undo]");
  const button = closeButton ?? snoozeButton ?? undoButton;
  if (!button) return;
  button.disabled = true;
  try {
    if (closeButton) {
      await request(`/api/reminders/${closeButton.dataset.close}/close`, { method: "POST" });
    } else if (snoozeButton) {
      await request(`/api/reminders/${snoozeButton.dataset.snooze}/snooze`, {
        method: "POST",
        body: JSON.stringify({ minutes: 10 }),
      });
    } else {
      await request(`/api/calendar/undo/${undoButton.dataset.undo}`, { method: "POST" });
    }
    await loadReceipts();
  } catch (error) {
    button.disabled = false;
    connection.textContent = error instanceof Error ? error.message : "操作失败";
    connection.classList.remove("online");
  }
});

document.addEventListener("visibilitychange", () => {
  if (document.hidden && holding) endHold(new Event("visibilitychange", { cancelable: true }));
});

const eventSource = new EventSource("/api/receipts/stream");
eventSource.addEventListener("open", () => {
  connection.textContent = "实时连接";
  connection.classList.add("online");
});
eventSource.addEventListener("receipt", (event) => {
  const receipt = JSON.parse(event.data);
  if (!receipts.some((item) => item.id === receipt.id)) receipts.push(receipt);
  renderReceipts();
  if (activeTab !== "messages") {
    unreadCount += 1;
    unread.textContent = unreadCount > 9 ? "9+" : String(unreadCount);
    unread.hidden = false;
  }
});
eventSource.addEventListener("error", () => {
  connection.textContent = "重新连接中";
  connection.classList.remove("online");
});

setState("idle");
void loadReceipts();
