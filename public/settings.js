const form = document.querySelector("#linx-settings-form");
const apiKeyInput = document.querySelector("#api-key");
const agentIdInput = document.querySelector("#agent-id");
const voiceIdInput = document.querySelector("#voice-id");
const saveButton = document.querySelector("#save-settings");
const copyTokenButton = document.querySelector("#copy-token");
const activateButton = document.querySelector("#activate-device");
const testButton = document.querySelector("#test-voice");
const restartNotice = document.querySelector("#restart-notice");
const toast = document.querySelector("#toast");
const bubbleLimitButtons = [...document.querySelectorAll("[data-bubble-limit]")];

let currentStatus = null;
let tokenCopied = false;
let toastTimer = null;

function currentBubbleLimit() {
  const value = Number.parseInt(localStorage.getItem("voiceBubbleLimit") ?? "3", 10);
  return [1, 3, 5].includes(value) ? value : 3;
}

function renderBubbleLimit(limit) {
  for (const button of bubbleLimitButtons) {
    button.setAttribute("aria-pressed", String(Number(button.dataset.bubbleLimit) === limit));
  }
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

function showToast(message) {
  toast.textContent = message;
  toast.classList.add("show");
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => toast.classList.remove("show"), 2600);
}

function setActionResult(element, message, error = false) {
  element.textContent = message;
  element.classList.toggle("error", error);
}

function maskIdentifier(value) {
  if (!value) return "—";
  if (value.length <= 14) return value;
  return `${value.slice(0, 7)}…${value.slice(-5)}`;
}

function renderStatus(status, fillForm = true) {
  currentStatus = status;
  if (fillForm) {
    agentIdInput.value = status.agentId ?? "";
    voiceIdInput.value = status.voiceId ?? "";
  }
  apiKeyInput.placeholder = status.apiKeyConfigured
    ? "已配置，留空则保持不变"
    : "输入灵矽 API Key";
  document.querySelector("#api-key-status").textContent = status.apiKeyConfigured
    ? "已保存在本机 .env，不会回显"
    : "尚未配置";

  const deviceReady = status.voiceEnabled && status.deviceTokenConfigured;
  const runtimeReady = status.runtimeVoiceReady === true;
  document.querySelector("#step-api").classList.toggle("complete", status.apiKeyConfigured);
  document.querySelector("#step-mcp").classList.toggle("complete", tokenCopied);
  document.querySelector("#step-agent").classList.toggle("complete", Boolean(status.agentId) || deviceReady);
  document.querySelector("#step-device").classList.toggle("complete", deviceReady);

  document.querySelector("#summary-api").textContent = status.apiKeyConfigured ? "已配置" : "未配置";
  document.querySelector("#summary-device").textContent = deviceReady ? "已配置" : "未配置";
  document.querySelector("#summary-device-id").textContent = maskIdentifier(status.deviceId);
  document.querySelector("#summary-device-id").title = status.deviceId || "";
  document.querySelector("#summary-runtime").textContent = runtimeReady ? "已加载" : "等待重启";

  const deviceStatus = document.querySelector("#device-status");
  deviceStatus.textContent = deviceReady ? "设备配置完成" : "未绑定设备";
  deviceStatus.classList.toggle("ready", deviceReady);
  const overall = document.querySelector("#overall-status");
  overall.textContent = runtimeReady ? "语音已加载" : deviceReady ? "配置待重启" : "尚未完成";
  overall.classList.toggle("ready", runtimeReady);
  testButton.disabled = !deviceReady;
}

async function copyText(text) {
  if (navigator.clipboard?.writeText) {
    await navigator.clipboard.writeText(text);
    return;
  }
  const textarea = document.createElement("textarea");
  textarea.value = text;
  textarea.style.position = "fixed";
  textarea.style.opacity = "0";
  document.body.append(textarea);
  textarea.select();
  const copied = document.execCommand("copy");
  textarea.remove();
  if (!copied) throw new Error("浏览器未允许复制，请检查剪贴板权限");
}

async function loadStatus(fillForm = true) {
  try {
    const status = await request("/api/settings/linx");
    renderStatus(status, fillForm);
  } catch (error) {
    document.querySelector("#overall-status").textContent = "读取失败";
    showToast(error instanceof Error ? error.message : "读取配置失败");
  }
}

form.addEventListener("submit", async (event) => {
  event.preventDefault();
  saveButton.disabled = true;
  try {
    const status = await request("/api/settings/linx", {
      method: "PUT",
      body: JSON.stringify({
        apiKey: apiKeyInput.value,
        agentId: agentIdInput.value,
        voiceId: voiceIdInput.value,
      }),
    });
    apiKeyInput.value = "";
    restartNotice.hidden = false;
    renderStatus({ ...status, runtimeVoiceReady: currentStatus?.runtimeVoiceReady }, true);
    showToast("配置已安全保存到本机");
  } catch (error) {
    showToast(error instanceof Error ? error.message : "保存失败");
  } finally {
    saveButton.disabled = false;
  }
});

copyTokenButton.addEventListener("click", async () => {
  copyTokenButton.disabled = true;
  const result = document.querySelector("#token-result");
  setActionResult(result, "正在获取临时 Token…");
  try {
    const data = await request("/api/settings/linx/mcp-token", { method: "POST" });
    await copyText(data.token);
    tokenCopied = true;
    document.querySelector("#step-mcp").classList.add("complete");
    const expiresAt = new Date(data.expiresAt).toLocaleString("zh-CN", { hour12: false });
    setActionResult(result, `已复制，过期时间：${expiresAt}`);
  } catch (error) {
    setActionResult(result, error instanceof Error ? error.message : "获取失败", true);
  } finally {
    copyTokenButton.disabled = false;
  }
});

activateButton.addEventListener("click", async () => {
  activateButton.disabled = true;
  activateButton.textContent = "正在申请设备…";
  try {
    const data = await request("/api/settings/linx/activate", { method: "POST" });
    const activationResult = document.querySelector("#activation-result");
    activationResult.hidden = false;
    document.querySelector("#activation-code").textContent = data.activationCode ?? "设备可能已绑定";
    document.querySelector("#activation-message").textContent = data.activationMessage
      ?? (data.activationCode
        ? "请到目标 Agent 的设备管理中输入此激活码。"
        : "OTA 未返回激活码，可直接播放连接测试；如失败，请检查控制台中的设备绑定。");
    restartNotice.hidden = false;
    await loadStatus(false);
  } catch (error) {
    showToast(error instanceof Error ? error.message : "设备激活失败");
  } finally {
    activateButton.disabled = false;
    activateButton.textContent = "生成或刷新激活码";
  }
});

testButton.addEventListener("click", async () => {
  testButton.disabled = true;
  const result = document.querySelector("#test-result");
  setActionResult(result, "正在连接并等待语音回复…");
  try {
    const data = await request("/api/settings/linx/test", { method: "POST" });
    setActionResult(result, `播放成功 · ${data.format.toUpperCase()} · ${data.audioBytes} 字节`);
  } catch (error) {
    setActionResult(result, error instanceof Error ? error.message : "连接测试失败", true);
  } finally {
    testButton.disabled = !(currentStatus?.voiceEnabled && currentStatus?.deviceTokenConfigured);
  }
});

for (const button of bubbleLimitButtons) {
  button.addEventListener("click", () => {
    const limit = Number(button.dataset.bubbleLimit);
    localStorage.setItem("voiceBubbleLimit", String(limit));
    renderBubbleLimit(limit);
    showToast(`语音页最多显示 ${limit} 条对话`);
  });
}

renderBubbleLimit(currentBubbleLimit());
void loadStatus();
