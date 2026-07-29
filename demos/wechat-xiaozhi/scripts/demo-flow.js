const baseUrl = process.env.BASE_URL || "http://localhost:8787";
const apiKey = process.env.DEVICE_API_KEY || "change-me";
const headers = {
  authorization: `Bearer ${apiKey}`,
  "content-type": "application/json"
};

async function request(path, body) {
  const response = await fetch(`${baseUrl}${path}`, {
    method: body === undefined ? "GET" : "POST",
    headers,
    body: body === undefined ? undefined : JSON.stringify(body)
  });
  const result = await response.json();
  if (!response.ok) throw new Error(`${response.status}: ${JSON.stringify(result)}`);
  return result;
}

console.log("1. 为小智生成微信绑定码");
const bindingCode = await request("/api/binding-codes", {
  deviceId: "xiaozhi-demo-01"
});
console.log(bindingCode);

console.log("2. 模拟微信用户发送绑定码");
console.log(await request("/api/demo/bind", {
  code: bindingCode.code,
  openId: "demo-openid"
}));

console.log("3. 创建一条立即发送的提醒");
const reminder = await request("/api/reminders", {
  deviceId: "xiaozhi-demo-01",
  title: "Demo：喝一杯水",
  dueAt: new Date(Date.now() + 1000).toISOString()
});
console.log(reminder);

await new Promise((resolve) => setTimeout(resolve, 1800));

console.log("4. 查看发送和回执状态");
const state = await request("/api/state");
console.log({
  binding: state.bindings["xiaozhi-demo-01"],
  reminder: state.reminders[reminder.id],
  outbound: state.outbound.at(-1)
});
