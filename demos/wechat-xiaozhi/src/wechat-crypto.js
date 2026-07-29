import crypto from "node:crypto";

function sha1(parts) {
  return crypto
    .createHash("sha1")
    .update(parts.map(String).sort().join(""))
    .digest("hex");
}

export function verifyPlainSignature({ token, timestamp, nonce, signature }) {
  if (!signature) return false;
  const expected = sha1([token, timestamp, nonce]);
  const left = Buffer.from(expected);
  const right = Buffer.from(String(signature));
  return left.length === right.length && crypto.timingSafeEqual(left, right);
}

export function messageSignature({ token, timestamp, nonce, encrypted }) {
  return sha1([token, timestamp, nonce, encrypted]);
}

export function verifyMessageSignature({
  token,
  timestamp,
  nonce,
  encrypted,
  signature
}) {
  if (!signature) return false;
  const expected = messageSignature({ token, timestamp, nonce, encrypted });
  const left = Buffer.from(expected);
  const right = Buffer.from(String(signature));
  return left.length === right.length && crypto.timingSafeEqual(left, right);
}

function decodeAesKey(encodingAesKey) {
  const key = Buffer.from(`${encodingAesKey}=`, "base64");
  if (key.length !== 32) {
    throw new Error("WECHAT_AES_KEY 必须是公众平台提供的 43 字符 EncodingAESKey");
  }
  return key;
}

function pkcs7Pad(buffer, blockSize = 32) {
  const padding = blockSize - (buffer.length % blockSize || blockSize);
  const size = padding === 0 ? blockSize : padding;
  return Buffer.concat([buffer, Buffer.alloc(size, size)]);
}

function pkcs7Unpad(buffer, blockSize = 32) {
  if (buffer.length === 0) throw new Error("微信密文为空");
  const size = buffer.at(-1);
  if (size < 1 || size > blockSize || size > buffer.length) {
    throw new Error("微信密文 PKCS#7 填充无效");
  }
  for (let index = buffer.length - size; index < buffer.length; index += 1) {
    if (buffer[index] !== size) throw new Error("微信密文 PKCS#7 填充无效");
  }
  return buffer.subarray(0, buffer.length - size);
}

export function encryptWechatMessage({ xml, appId, encodingAesKey, randomBytes = crypto.randomBytes }) {
  const key = decodeAesKey(encodingAesKey);
  const xmlBuffer = Buffer.from(xml);
  const length = Buffer.alloc(4);
  length.writeUInt32BE(xmlBuffer.length);
  const plain = Buffer.concat([
    randomBytes(16),
    length,
    xmlBuffer,
    Buffer.from(appId)
  ]);
  const cipher = crypto.createCipheriv("aes-256-cbc", key, key.subarray(0, 16));
  cipher.setAutoPadding(false);
  return Buffer.concat([cipher.update(pkcs7Pad(plain)), cipher.final()]).toString("base64");
}

export function decryptWechatMessage({ encrypted, appId, encodingAesKey }) {
  const key = decodeAesKey(encodingAesKey);
  const decipher = crypto.createDecipheriv("aes-256-cbc", key, key.subarray(0, 16));
  decipher.setAutoPadding(false);
  const padded = Buffer.concat([
    decipher.update(Buffer.from(encrypted, "base64")),
    decipher.final()
  ]);
  const plain = pkcs7Unpad(padded);
  if (plain.length < 20) throw new Error("微信密文长度无效");
  const xmlLength = plain.readUInt32BE(16);
  const xmlStart = 20;
  const xmlEnd = xmlStart + xmlLength;
  if (xmlEnd > plain.length) throw new Error("微信密文中的消息长度无效");
  const xml = plain.subarray(xmlStart, xmlEnd).toString();
  const embeddedAppId = plain.subarray(xmlEnd).toString();
  if (appId && embeddedAppId !== appId) throw new Error("微信密文 AppID 不匹配");
  return { xml, appId: embeddedAppId };
}
