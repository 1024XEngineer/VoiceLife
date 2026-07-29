function escapeCdata(value) {
  return String(value ?? "").replaceAll("]]>", "]]]]><![CDATA[>");
}

function decodeXml(value) {
  return String(value)
    .replaceAll("&lt;", "<")
    .replaceAll("&gt;", ">")
    .replaceAll("&quot;", "\"")
    .replaceAll("&apos;", "'")
    .replaceAll("&amp;", "&");
}

export function xmlField(xml, name) {
  const safeName = name.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
  const pattern = new RegExp(
    `<${safeName}>(?:<!\\[CDATA\\[([\\s\\S]*?)\\]\\]>|([^<]*))<\\/${safeName}>`,
    "i"
  );
  const match = String(xml).match(pattern);
  if (!match) return "";
  return decodeXml(match[1] ?? match[2] ?? "");
}

export function parseWechatXml(xml) {
  const names = [
    "ToUserName",
    "FromUserName",
    "CreateTime",
    "MsgType",
    "Content",
    "MsgId",
    "MediaId",
    "Format",
    "Recognition",
    "Event",
    "EventKey",
    "Ticket",
    "Status",
    "Encrypt"
  ];
  return Object.fromEntries(names.map((name) => [name, xmlField(xml, name)]));
}

export function textReplyXml({ to, from, content, timestamp = Math.floor(Date.now() / 1000) }) {
  return `<xml>
<ToUserName><![CDATA[${escapeCdata(to)}]]></ToUserName>
<FromUserName><![CDATA[${escapeCdata(from)}]]></FromUserName>
<CreateTime>${timestamp}</CreateTime>
<MsgType><![CDATA[text]]></MsgType>
<Content><![CDATA[${escapeCdata(content)}]]></Content>
</xml>`;
}

export function encryptedReplyXml({ encrypted, signature, timestamp, nonce }) {
  return `<xml>
<Encrypt><![CDATA[${escapeCdata(encrypted)}]]></Encrypt>
<MsgSignature><![CDATA[${escapeCdata(signature)}]]></MsgSignature>
<TimeStamp>${timestamp}</TimeStamp>
<Nonce><![CDATA[${escapeCdata(nonce)}]]></Nonce>
</xml>`;
}
