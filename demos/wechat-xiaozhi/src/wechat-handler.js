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
import {
  deactivateWechatBinding,
  handleWechatBindingEvent,
  handleWechatBindingText,
  handleWechatVoice,
  helpText
} from "./wechat-domain.js";

function responseText(message, content) {
  return textReplyXml({
    to: message.FromUserName,
    from: message.ToUserName,
    content
  });
}

export async function handleWechatMessage(message, { bindingHandler, receiptService }) {
  const type = message.MsgType.toLowerCase();
  if (type === "text") {
    return responseText(message, handleWechatBindingText({
      channelAccountId: message.ToUserName,
      content: message.Content,
      eventId: message.MsgId,
      openId: message.FromUserName,
      bindingHandler
    }));
  }

  if (type === "voice") {
    return responseText(message, handleWechatVoice({
      recognition: message.Recognition,
      mediaId: message.MediaId
    }));
  }

  if (type === "event") {
    const event = message.Event.toLowerCase();
    if (event === "templatesendjobfinish") {
      receiptService.recordTemplateReceipt({
        messageId: message.MsgId,
        status: message.Status,
        openId: message.FromUserName
      });
      return "";
    }
    if (event === "subscribe" || event === "scan") {
      return responseText(message, handleWechatBindingEvent({
        channelAccountId: message.ToUserName,
        eventId: message.MsgId || `${message.CreateTime}:${event}`,
        eventKey: message.EventKey,
        openId: message.FromUserName,
        bindingHandler
      }));
    }
    if (event === "unsubscribe") {
      deactivateWechatBinding({
        channelAccountId: message.ToUserName,
        eventId: message.MsgId || `${message.CreateTime}:${event}`,
        openId: message.FromUserName,
        bindingHandler
      });
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

export async function processWechatCallback({
  url,
  body,
  config,
  bindingHandler,
  receiptService
}) {
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

  const reply = await handleWechatMessage(parseWechatXml(plainXml), {
    bindingHandler,
    receiptService
  });
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
