import crypto from "node:crypto";
import {
  decryptWechatMessage,
  encryptWechatMessage,
  messageSignature,
  verifyMessageSignature,
  verifyPlainSignature
} from "./wechat-crypto.js";
import {
  encryptedReplyXml,
  parseWechatXml,
  textReplyXml,
  xmlField
} from "./xml.js";

function responseText(message, content) {
  return textReplyXml({
    to: message.FromUserName,
    from: message.ToUserName,
    content
  });
}

function bindingCodeFromEvent(message) {
  if (!message.EventKey) return "";
  return message.EventKey.replace(/^qrscene_/i, "");
}

function helpText() {
  return [
    "VoiceLife 微信 Demo",
    "发送“绑定 ABC123”绑定小智设备",
    "发送“关闭”关闭最近提醒",
    "发送“推迟10分钟”推迟最近提醒"
  ].join("\n");
}

export async function handleWechatMessage(message, service) {
  const type = message.MsgType.toLowerCase();
  if (type === "text") {
    const content = message.Content.trim();
    const bindMatch = content.match(/^绑定\s*([A-Z0-9]{4,32})$/i);
    if (bindMatch) {
      try {
        const binding = service.bindOpenId(bindMatch[1], message.FromUserName);
        return responseText(message, `绑定成功：${binding.deviceId}`);
      } catch (error) {
        return responseText(message, `绑定失败：${error.message}`);
      }
    }
    if (/^(关闭|完成|取消提醒)$/i.test(content)) {
      try {
        const reminder = service.dismiss({ openId: message.FromUserName });
        return responseText(message, `已关闭：${reminder.title}`);
      } catch (error) {
        return responseText(message, error.message);
      }
    }
    const snoozeMatch = content.match(/^推迟\s*(\d{1,4})\s*分钟$/);
    if (snoozeMatch) {
      try {
        const reminder = service.snooze({
          openId: message.FromUserName,
          minutes: Number(snoozeMatch[1])
        });
        return responseText(message, `已推迟到 ${reminder.dueAt}`);
      } catch (error) {
        return responseText(message, `推迟失败：${error.message}`);
      }
    }
    return responseText(message, helpText());
  }

  if (type === "voice") {
    if (message.Recognition) {
      return responseText(message, `已收到语音识别文本：${message.Recognition}\nDemo 下一步可把它交给日程解析器。`);
    }
    return responseText(message, `已收到语音，MediaId=${message.MediaId || "未知"}；当前 Demo 未配置语音转写。`);
  }

  if (type === "event") {
    const event = message.Event.toLowerCase();
    if (event === "templatesendjobfinish") {
      service.recordTemplateReceipt({
        messageId: message.MsgId,
        status: message.Status,
        openId: message.FromUserName
      });
      return "";
    }
    if (event === "subscribe" || event === "scan") {
      const code = bindingCodeFromEvent(message);
      if (!code) return responseText(message, helpText());
      try {
        const binding = service.bindOpenId(code, message.FromUserName);
        return responseText(message, `扫码绑定成功：${binding.deviceId}`);
      } catch (error) {
        return responseText(message, `扫码绑定失败：${error.message}`);
      }
    }
    if (event === "unsubscribe") {
      const binding = service.findDeviceByOpenId(message.FromUserName);
      if (binding) {
        service.store.mutate((state) => {
          state.bindings[binding.deviceId].active = false;
        });
      }
      return "";
    }
  }

  return responseText(message, helpText());
}

function requireAes(config) {
  if (!config.aesKey || !config.appId) {
    throw new Error("安全模式需要 WECHAT_AES_KEY 和 WECHAT_APP_ID");
  }
}

export async function verifyWechatUrl(url, config) {
  const timestamp = url.searchParams.get("timestamp") || "";
  const nonce = url.searchParams.get("nonce") || "";
  const echo = url.searchParams.get("echostr") || "";
  const encryptedSignature = url.searchParams.get("msg_signature");
  if (encryptedSignature) {
    requireAes(config);
    if (!verifyMessageSignature({
      token: config.token,
      timestamp,
      nonce,
      encrypted: echo,
      signature: encryptedSignature
    })) {
      throw new Error("微信安全模式签名校验失败");
    }
    return decryptWechatMessage({
      encrypted: echo,
      appId: config.appId,
      encodingAesKey: config.aesKey
    }).xml;
  }
  if (!verifyPlainSignature({
    token: config.token,
    timestamp,
    nonce,
    signature: url.searchParams.get("signature") || ""
  })) {
    throw new Error("微信明文签名校验失败");
  }
  return echo;
}

export async function processWechatCallback({ url, body, config, service }) {
  const timestamp = url.searchParams.get("timestamp") || "";
  const nonce = url.searchParams.get("nonce") || "";
  const encrypted = xmlField(body, "Encrypt");
  let plainXml = body;
  let safeMode = false;

  if (encrypted) {
    safeMode = true;
    requireAes(config);
    if (!verifyMessageSignature({
      token: config.token,
      timestamp,
      nonce,
      encrypted,
      signature: url.searchParams.get("msg_signature") || ""
    })) {
      throw new Error("微信安全模式消息签名校验失败");
    }
    plainXml = decryptWechatMessage({
      encrypted,
      appId: config.appId,
      encodingAesKey: config.aesKey
    }).xml;
  } else if (!verifyPlainSignature({
    token: config.token,
    timestamp,
    nonce,
    signature: url.searchParams.get("signature") || ""
  })) {
    throw new Error("微信明文消息签名校验失败");
  }

  const reply = await handleWechatMessage(parseWechatXml(plainXml), service);
  if (!reply || !safeMode) return reply || "success";

  const responseTimestamp = String(Math.floor(Date.now() / 1000));
  const responseNonce = crypto.randomBytes(8).toString("hex");
  const responseEncrypted = encryptWechatMessage({
    xml: reply,
    appId: config.appId,
    encodingAesKey: config.aesKey
  });
  return encryptedReplyXml({
    encrypted: responseEncrypted,
    signature: messageSignature({
      token: config.token,
      timestamp: responseTimestamp,
      nonce: responseNonce,
      encrypted: responseEncrypted
    }),
    timestamp: responseTimestamp,
    nonce: responseNonce
  });
}
