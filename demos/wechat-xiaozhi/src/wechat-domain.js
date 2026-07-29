export function helpText() {
  return [
    "VoiceLife 微信 Demo",
    "发送“绑定 ABC123”绑定小智设备",
    "发送“关闭”关闭最近提醒",
    "发送“推迟10分钟”推迟最近提醒"
  ].join("\n");
}

export function bindingCodeFromEvent(eventKey) {
  return String(eventKey || "").replace(/^qrscene_/i, "");
}

export function handleWechatText({ content, openId, service }) {
  const text = String(content || "").trim();
  const bindMatch = text.match(/^绑定\s*([A-Z0-9]{4,32})$/i);
  if (bindMatch) {
    try {
      const binding = service.bindOpenId(bindMatch[1], openId);
      return `绑定成功：${binding.deviceId}`;
    } catch (error) {
      return `绑定失败：${error.message}`;
    }
  }
  if (/^(关闭|完成|取消提醒)$/i.test(text)) {
    try {
      const reminder = service.dismiss({ openId });
      return `已关闭：${reminder.title}`;
    } catch (error) {
      return error.message;
    }
  }
  const snoozeMatch = text.match(/^推迟\s*(\d{1,4})\s*分钟$/);
  if (snoozeMatch) {
    try {
      const reminder = service.snooze({
        openId,
        minutes: Number(snoozeMatch[1])
      });
      return `已推迟到 ${reminder.dueAt}`;
    } catch (error) {
      return `推迟失败：${error.message}`;
    }
  }
  return helpText();
}

export function handleWechatVoice({ recognition, mediaId }) {
  if (recognition) {
    return `已收到语音识别文本：${recognition}\nDemo 下一步可把它交给日程解析器。`;
  }
  return `已收到语音，MediaId=${mediaId || "未知"}；当前 Demo 未配置语音转写。`;
}

export function handleWechatBindingEvent({ eventKey, openId, service }) {
  const code = bindingCodeFromEvent(eventKey);
  if (!code) return helpText();
  try {
    const binding = service.bindOpenId(code, openId);
    return `扫码绑定成功：${binding.deviceId}`;
  } catch (error) {
    return `扫码绑定失败：${error.message}`;
  }
}

export function deactivateWechatBinding({ openId, service }) {
  const binding = service.findDeviceByOpenId(openId);
  if (!binding) return false;
  service.store.mutate((state) => {
    state.bindings[binding.deviceId].active = false;
  });
  return true;
}
