import assert from "node:assert/strict";
import crypto from "node:crypto";
import test from "node:test";
import {
  decryptWechatMessage,
  encryptWechatMessage,
  messageSignature,
  verifyMessageSignature,
  verifyPlainSignature
} from "../src/wechat-crypto.js";

const aesKey = Buffer.alloc(32, 7).toString("base64").replace(/=$/, "");

test("明文签名按字典序 SHA-1 校验", () => {
  const token = "demo-token";
  const timestamp = "1700000000";
  const nonce = "abc";
  const signature = crypto
    .createHash("sha1")
    .update([token, timestamp, nonce].sort().join(""))
    .digest("hex");
  assert.equal(verifyPlainSignature({ token, timestamp, nonce, signature }), true);
  assert.equal(verifyPlainSignature({ token, timestamp, nonce, signature: "bad" }), false);
});

test("微信 AES 消息可以完整加解密并校验 AppID", () => {
  const input = "<xml><Content><![CDATA[你好]]></Content></xml>";
  const encrypted = encryptWechatMessage({
    xml: input,
    appId: "wx-demo",
    encodingAesKey: aesKey,
    randomBytes: () => Buffer.alloc(16, 1)
  });
  assert.deepEqual(
    decryptWechatMessage({
      encrypted,
      appId: "wx-demo",
      encodingAesKey: aesKey
    }),
    { xml: input, appId: "wx-demo" }
  );
  assert.throws(
    () => decryptWechatMessage({
      encrypted,
      appId: "wrong",
      encodingAesKey: aesKey
    }),
    /AppID 不匹配/
  );
});

test("安全模式消息签名可验证", () => {
  const values = {
    token: "token",
    timestamp: "1700000000",
    nonce: "nonce",
    encrypted: "ciphertext"
  };
  const signature = messageSignature(values);
  assert.equal(verifyMessageSignature({ ...values, signature }), true);
  assert.equal(verifyMessageSignature({ ...values, signature: `${signature}0` }), false);
});
