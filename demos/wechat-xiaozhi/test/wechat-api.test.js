import assert from "node:assert/strict";
import test from "node:test";
import { WechatApi } from "../src/wechat-api.js";

test("模板消息 msgid 以精确字符串保存，避免大整数精度丢失", async () => {
  const responses = [
    new Response(JSON.stringify({
      access_token: "token",
      expires_in: 7200
    })),
    new Response(
      '{"errcode":0,"errmsg":"ok","msgid":4625712877545553920}',
      { status: 200, headers: { "content-type": "application/json" } }
    )
  ];
  const api = new WechatApi(
    {
      appId: "wx-demo",
      appSecret: "secret",
      outboundMode: "live",
      templateId: "template",
      templateFields: {
        title: "thing1",
        time: "time2",
        status: "thing3"
      },
      detailUrl: ""
    },
    {
      fetchImpl: async () => responses.shift(),
      now: () => 1
    }
  );

  const result = await api.sendTemplate({
    openId: "openid",
    reminder: {
      id: "reminder",
      title: "喝水",
      dueAt: "2026-07-29T15:00:00+08:00"
    }
  });

  assert.equal(result.msgid, "4625712877545553920");
});

test("模板消息详情地址包含签名操作令牌", async () => {
  let sentBody;
  const responses = [
    new Response(JSON.stringify({ access_token: "token", expires_in: 7200 })),
    new Response('{"errcode":0,"errmsg":"ok","msgid":1}')
  ];
  const api = new WechatApi(
    {
      appId: "wx-demo",
      appSecret: "secret",
      outboundMode: "live",
      templateId: "template",
      templateFields: { title: "thing1", time: "time2", status: "thing3" },
      detailUrl: "https://example.com/reminders/action",
      actionTokenSecret: "action-secret",
      actionTokenTtlSeconds: 600
    },
    {
      fetchImpl: async (url, options) => {
        if (options?.body) sentBody = JSON.parse(options.body);
        return responses.shift();
      },
      now: () => 1_000
    }
  );

  await api.sendTemplate({
    openId: "openid",
    reminder: {
      id: "reminder",
      title: "喝水",
      dueAt: "2026-07-29T15:00:00+08:00"
    }
  });

  const detail = new URL(sentBody.url);
  assert.equal(detail.pathname, "/reminders/action");
  assert.ok(detail.searchParams.get("token"));
  assert.equal(detail.searchParams.has("reminder_id"), false);
});
