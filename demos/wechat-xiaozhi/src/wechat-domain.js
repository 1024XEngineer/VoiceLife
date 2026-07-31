import { BindingEventType } from "./binding-handler.js";

function wechatActor(openId) {
  return {
    platform: "wechat-official",
    userId: String(openId || "")
  };
}

function normalizedEventId(eventId, type, openId, value = "") {
  return String(eventId || `wechat-official:${type}:${openId}:${value}`);
}

export function helpText() {
  return [
    "VoiceLife 微信绑定入口",
    "发送“绑定 ABC123”绑定小智设备"
  ].join("\n");
}

export function bindingCodeFromEvent(eventKey) {
  return String(eventKey || "").replace(/^qrscene_/i, "");
}

export function handleWechatBindingText({
  content,
  channelAccountId = "wechat-official",
  eventId,
  openId,
  bindingHandler
}) {
  const text = String(content || "").trim();
  const bindMatch = text.match(/^绑定\s*([A-Z0-9]{4,32})$/i);
  if (bindMatch) {
    try {
      const result = bindingHandler.handle({
        type: BindingEventType.REQUESTED,
        channelAccountId,
        eventId: normalizedEventId(eventId, "message", openId, bindMatch[1]),
        actor: wechatActor(openId),
        pairingCode: bindMatch[1]
      });
      return `绑定成功：${result.binding.deviceId}`;
    } catch (error) {
      return `绑定失败：${error.message}`;
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

export function handleWechatBindingEvent({
  eventId,
  eventKey,
  channelAccountId = "wechat-official",
  openId,
  bindingHandler
}) {
  const code = bindingCodeFromEvent(eventKey);
  if (!code) return helpText();
  try {
    const result = bindingHandler.handle({
      type: BindingEventType.REQUESTED,
      channelAccountId,
      eventId: normalizedEventId(eventId, "scan", openId, eventKey),
      actor: wechatActor(openId),
      pairingCode: code
    });
    return `扫码绑定成功：${result.binding.deviceId}`;
  } catch (error) {
    return `扫码绑定失败：${error.message}`;
  }
}

export function deactivateWechatBinding({
  channelAccountId = "wechat-official",
  eventId,
  openId,
  bindingHandler
}) {
  const result = bindingHandler.handle({
    type: BindingEventType.DEACTIVATED,
    channelAccountId,
    eventId: normalizedEventId(eventId, "unsubscribe", openId),
    actor: wechatActor(openId)
  });
  return result.status === "deactivated";
}
