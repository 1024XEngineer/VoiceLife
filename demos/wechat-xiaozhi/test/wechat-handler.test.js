import assert from "node:assert/strict";
import crypto from "node:crypto";
import test from "node:test";
import { VoiceLifeService } from "../src/service.js";
import { MemoryStore } from "../src/store.js";
import {
  processWechatCallback,
  verifyWechatUrl
} from "../src/wechat-handler.js";
import {
  decryptWechatMessage,
  encryptWechatMessage,
  messageSignature
} from "../src/wechat-crypto.js";
import { parseWechatXml, xmlField } from "../src/xml.js";

function plainSignature(token, timestamp, nonce) {
  return crypto
    .createHash("sha1")
    .update([token, timestamp, nonce].sort().join(""))
    .digest("hex");
}

function fixture() {
  const store = new MemoryStore();
  const service = new VoiceLifeService({
    store,
    wechatApi: { sendTemplate: async () => ({ msgid: "1" }) },
    now: () => Date.parse("2026-07-29T10:00:00+08:00")
  });
  return { store, service };
}

test("公众号 URL 明文校验返回 echostr", async () => {
  const token = "token";
  const timestamp = "1";
  const nonce = "2";
  const url = new URL("https://demo.example/wechat/callback");
  url.searchParams.set("timestamp", timestamp);
  url.searchParams.set("nonce", nonce);
  url.searchParams.set("echostr", "hello");
  url.searchParams.set("signature", plainSignature(token, timestamp, nonce));
  assert.equal(await verifyWechatUrl(url, { token }), "hello");
});

test("明文公众号消息可以完成设备绑定", async () => {
  const { service } = fixture();
  const binding = service.createBindingCode("xiaozhi-01");
  const token = "token";
  const timestamp = "1";
  const nonce = "2";
  const url = new URL("https://demo.example/wechat/callback");
  url.searchParams.set("timestamp", timestamp);
  url.searchParams.set("nonce", nonce);
  url.searchParams.set("signature", plainSignature(token, timestamp, nonce));
  const body = `<xml>
<ToUserName><![CDATA[gh_demo]]></ToUserName>
<FromUserName><![CDATA[openid-01]]></FromUserName>
<CreateTime>1</CreateTime>
<MsgType><![CDATA[text]]></MsgType>
<Content><![CDATA[绑定 ${binding.code}]]></Content>
<MsgId>100</MsgId>
</xml>`;
  const reply = await processWechatCallback({
    url,
    body,
    config: { token },
    service
  });
  assert.match(xmlField(reply, "Content"), /绑定成功/);
  assert.equal(service.findDeviceByOpenId("openid-01").deviceId, "xiaozhi-01");
});

test("安全模式回调可以解密、处理并加密回复", async () => {
  const { service } = fixture();
  const token = "token";
  const appId = "wx-demo";
  const aesKey = Buffer.alloc(32, 9).toString("base64").replace(/=$/, "");
  const plain = `<xml>
<ToUserName><![CDATA[gh_demo]]></ToUserName>
<FromUserName><![CDATA[openid-01]]></FromUserName>
<CreateTime>1</CreateTime>
<MsgType><![CDATA[text]]></MsgType>
<Content><![CDATA[帮助]]></Content>
<MsgId>101</MsgId>
</xml>`;
  const encrypted = encryptWechatMessage({
    xml: plain,
    appId,
    encodingAesKey: aesKey
  });
  const timestamp = "1";
  const nonce = "2";
  const url = new URL("https://demo.example/wechat/callback");
  url.searchParams.set("timestamp", timestamp);
  url.searchParams.set("nonce", nonce);
  url.searchParams.set(
    "msg_signature",
    messageSignature({ token, timestamp, nonce, encrypted })
  );
  const body = `<xml><Encrypt><![CDATA[${encrypted}]]></Encrypt></xml>`;
  const response = await processWechatCallback({
    url,
    body,
    config: { token, appId, aesKey },
    service
  });
  const responseEncrypted = xmlField(response, "Encrypt");
  const decrypted = decryptWechatMessage({
    encrypted: responseEncrypted,
    appId,
    encodingAesKey: aesKey
  }).xml;
  assert.match(parseWechatXml(decrypted).Content, /VoiceLife 微信 Demo/);
});
