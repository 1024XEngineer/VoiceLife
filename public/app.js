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

const SpeechRecognition = window.SpeechRecognition ?? window.webkitSpeechRecognition;
let recognition = null;
let holding = false;
let busy = false;
let sent = false;
let finalTranscript = "";
let interimTranscript = "";
let activePointerId = null;
let resetTimer = null;
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
    button: "正在回复",
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

async function streamVoiceInteraction(text) {
  const response = await fetch("/api/voice/interact/stream", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ text }),
  });
  if (!response.ok) {
    const body = await response.json();
    throw new Error(body.error ?? "语音服务暂时不可用");
  }
  if (!response.body) throw new Error("浏览器无法读取语音响应流");

  const reader = response.body.getReader();
  const decoder = new TextDecoder();
  let buffer = "";
  let completeEvent = null;
  let assistantBubble = null;
  let actionResultShown = false;
  const handleLine = (line) => {
    if (!line.trim()) return;
    const event = JSON.parse(line);
    if (event.type === "message") {
      setState("replying");
      if (event.receipt?.type === "calendar_query") {
        actionResultShown = true;
        renderScheduleCard(event.receipt);
      } else if (!actionResultShown && event.text) {
        if (!assistantBubble?.isConnected) assistantBubble = addConversationBubble("assistant", event.text);
        else {
          assistantBubble.textContent = appendReplyText(assistantBubble.textContent, event.text);
          scheduleConversationBubbleDismissal(assistantBubble);
        }
      }
      return;
    }
    if (event.type === "error") throw new Error(event.error ?? "语音交互失败");
    if (event.type === "complete") completeEvent = event;
  };

  while (true) {
    const { done, value } = await reader.read();
    buffer += decoder.decode(value ?? new Uint8Array(), { stream: !done });
    const lines = buffer.split("\n");
    buffer = lines.pop() ?? "";
    for (const line of lines) handleLine(line);
    if (done) break;
  }
  if (buffer.trim()) handleLine(buffer);
  return completeEvent;
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

function createRecognition() {
  const instance = new SpeechRecognition();
  instance.lang = "zh-CN";
  instance.continuous = true;
  instance.interimResults = true;
  instance.maxAlternatives = 1;

  instance.addEventListener("result", (event) => {
    interimTranscript = "";
    for (let index = event.resultIndex; index < event.results.length; index += 1) {
      const result = event.results[index];
      const text = result[0]?.transcript ?? "";
      if (result.isFinal) finalTranscript += text;
      else interimTranscript += text;
    }
  });

  instance.addEventListener("error", (event) => {
    if (event.error === "aborted" && !holding) return;
    holding = false;
    busy = false;
    sent = true;
    const denied = event.error === "not-allowed" || event.error === "service-not-allowed";
    setState("error", {
      status: denied ? "需要麦克风权限" : "没有听清，请再试一次",
      hint: denied ? "请在浏览器地址栏允许使用麦克风" : "按住按钮后靠近麦克风说话",
    });
    scheduleIdle(3200);
  });

  instance.addEventListener("end", () => {
    recognition = null;
    if (holding) {
      try {
        recognition = createRecognition();
        recognition.start();
      } catch {
        holding = false;
        busy = false;
        setState("error", { status: "录音意外中断" });
        scheduleIdle();
      }
      return;
    }
    void sendTranscript();
  });

  return instance;
}

async function sendTranscript() {
  if (sent) return;
  sent = true;
  const text = `${finalTranscript} ${interimTranscript}`.trim();
  if (!text) {
    busy = false;
    setState("error", {
      status: "没有听到内容",
      hint: "请按住按钮后再开始说话",
    });
    scheduleIdle();
    return;
  }

  addConversationBubble("user", text);
  setState("processing");
  try {
    setTimeout(() => {
      if (busy) setState("replying");
    }, 450);
    await streamVoiceInteraction(text);
    setState("success", {
      status: "助手已回复",
    });
  } catch (error) {
    setState("error", {
      status: "语音服务暂时不可用",
      hint: error instanceof Error ? error.message : "请稍后重试",
    });
  } finally {
    busy = false;
    scheduleIdle(3000);
  }
}

function beginHold(event) {
  if (busy || holding) return;
  clearTimeout(resetTimer);
  if (!SpeechRecognition) {
    setState("error", {
      status: "当前浏览器不支持语音识别",
      hint: "请使用最新版 Chrome 打开此页面",
    });
    scheduleIdle(3500);
    return;
  }

  if (event instanceof PointerEvent) {
    if (!event.isPrimary || event.button !== 0) return;
    activePointerId = event.pointerId;
    talkButton.setPointerCapture?.(event.pointerId);
  }

  event.preventDefault();
  holding = true;
  busy = true;
  sent = false;
  finalTranscript = "";
  interimTranscript = "";
  navigator.vibrate?.(18);
  setState("listening");

  try {
    recognition = createRecognition();
    recognition.start();
  } catch {
    holding = false;
    busy = false;
    recognition = null;
    setState("error", {
      status: "麦克风没有启动",
      hint: "请检查浏览器的麦克风权限",
    });
    scheduleIdle(3000);
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
  try {
    recognition?.stop();
  } catch {
    recognition = null;
    void sendTranscript();
  }
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
