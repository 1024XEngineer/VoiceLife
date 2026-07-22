const messages = document.querySelector("#messages");
const empty = document.querySelector("#empty");
const connection = document.querySelector("#connection");
const clock = document.querySelector("#clock");
const toast = document.querySelector("#toast");

let receipts = [];
let toastTimer;

const labels = {
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

function showToast(message) {
  toast.textContent = message;
  toast.classList.add("show");
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => toast.classList.remove("show"), 2200);
}

function escapeHtml(value) {
  return String(value)
    .replaceAll("&", "&amp;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;")
    .replaceAll('"', "&quot;")
    .replaceAll("'", "&#039;");
}

function activeReceiptIds() {
  const states = new Map();
  for (const receipt of receipts) {
    const id = receipt.reminderId;
    if (!id) continue;
    if (receipt.type === "reminder_due") states.set(id, receipt.id);
    if (receipt.type === "reminder_closed" || receipt.type === "reminder_snoozed") states.set(id, null);
  }
  return new Set([...states.values()].filter(Boolean));
}

function render() {
  const active = activeReceiptIds();
  const undoneOperationIds = new Set(
    receipts
      .filter((receipt) => receipt.type === "calendar_undone")
      .map((receipt) => receipt.data?.operationId)
      .filter(Boolean),
  );
  empty.classList.toggle("hidden", receipts.length > 0);
  messages.innerHTML = [...receipts]
    .reverse()
    .map((receipt) => {
      const reminderActions = receipt.type === "reminder_due" && active.has(receipt.id)
        ? `<div class="message-actions">
            <button data-close="${escapeHtml(receipt.reminderId)}">知道了</button>
            <button data-snooze="${escapeHtml(receipt.reminderId)}">10 分钟后</button>
          </div>`
        : "";
      const undoOperationId = receipt.data?.undoOperationId;
      const undoActions = undoOperationId && !undoneOperationIds.has(undoOperationId)
        ? `<div class="message-actions">
            <button data-undo="${escapeHtml(undoOperationId)}">撤销（10 分钟内）</button>
          </div>`
        : "";
      const scheduleTag = receipt.data?.scheduleChanged === false
        ? '<span class="tag">日程未改变</span>'
        : "";
      return `<article class="message ${escapeHtml(receipt.type)}">
        <div class="message-header">
          <h3>${escapeHtml(labels[receipt.type] ?? receipt.title)}</h3>
          <time>${escapeHtml(new Date(receipt.createdAt).toLocaleString("zh-CN", { hour12: false }))}</time>
        </div>
        <p class="message-body">${escapeHtml(receipt.body)}</p>
        ${scheduleTag}
        ${reminderActions}
        ${undoActions}
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

async function loadReceipts() {
  const data = await request("/api/receipts");
  receipts = data.receipts;
  render();
}

async function loadClock() {
  const data = await request("/api/demo/clock");
  clock.textContent = new Date(data.now).toLocaleString("zh-CN", { hour12: false });
}

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
      showToast("提醒已关闭，日程保持不变");
    } else if (snoozeButton) {
      await request(`/api/reminders/${snoozeButton.dataset.snooze}/snooze`, {
        method: "POST",
        body: JSON.stringify({ minutes: 10 }),
      });
      showToast("已推迟 10 分钟");
    } else {
      await request(`/api/calendar/undo/${undoButton.dataset.undo}`, { method: "POST" });
      showToast("已撤销操作");
    }
    await loadReceipts();
  } catch (error) {
    showToast(error.message);
    button.disabled = false;
  }
});

document.querySelectorAll("[data-advance]").forEach((button) => {
  button.addEventListener("click", async () => {
    button.disabled = true;
    try {
      const data = await request("/api/demo/clock/advance", {
        method: "POST",
        body: JSON.stringify({ minutes: Number(button.dataset.advance) }),
      });
      showToast(`时间已推进，触发 ${data.pushed} 条提醒`);
      await Promise.all([loadClock(), loadReceipts()]);
    } catch (error) {
      showToast(error.message);
    } finally {
      button.disabled = false;
    }
  });
});

document.querySelector("#set-clock").addEventListener("submit", async (event) => {
  event.preventDefault();
  const input = document.querySelector("#clock-input");
  try {
    const iso = new Date(input.value).toISOString();
    const data = await request("/api/demo/clock/set", {
      method: "POST",
      body: JSON.stringify({ iso }),
    });
    showToast(`时间已设置，触发 ${data.pushed} 条提醒`);
    await Promise.all([loadClock(), loadReceipts()]);
  } catch (error) {
    showToast(error.message);
  }
});

document.querySelector("#reset-clock").addEventListener("click", async () => {
  await request("/api/demo/clock/reset", { method: "POST" });
  await loadClock();
  showToast("已恢复真实时间");
});

document.querySelector("#refresh").addEventListener("click", () => void loadReceipts());

const eventSource = new EventSource("/api/receipts/stream");
eventSource.addEventListener("open", () => {
  connection.textContent = "实时连接";
  connection.classList.add("online");
});
eventSource.addEventListener("receipt", (event) => {
  const receipt = JSON.parse(event.data);
  if (!receipts.some((item) => item.id === receipt.id)) receipts.push(receipt);
  render();
});
eventSource.addEventListener("error", () => {
  connection.textContent = "重新连接中";
  connection.classList.remove("online");
});

await Promise.all([loadReceipts(), loadClock()]);
setInterval(() => void loadClock(), 1000);
