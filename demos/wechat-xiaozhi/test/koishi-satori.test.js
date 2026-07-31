import assert from "node:assert/strict";
import test from "node:test";
import { Context } from "@koishijs/core";
import { HTTP } from "@koishijs/plugin-http";
import server from "@koishijs/plugin-server";
import satoriServer from "@koishijs/plugin-server-satori";
import { createImApplication } from "../src/im-application.js";
import { createVoiceLifeKoishiPlugin } from "../src/koishi-plugin.js";
import { VoiceLifeService } from "../src/service.js";
import { MemoryStore } from "../src/store.js";
import {
  createBindingServicePort,
  createReminderCommandPort
} from "../src/voicelife-ports.js";

test("Koishi 同时注册微信、VoiceLife 与 Satori 路由", async () => {
  const app = new Context();
  const config = {
    runtime: "koishi-satori",
    port: 0,
    baseUrl: "http://localhost",
    deviceApiKey: "api-key",
    demoMode: true,
    wechat: {
      account: "",
      token: "wechat-token",
      appId: "wx-demo",
      appSecret: "secret",
      aesKey: "",
      outboundMode: "mock",
      actionTokenSecret: "action-secret"
    },
    koishi: {
      selfUrl: "https://example.com",
      satoriPath: "/satori",
      satoriToken: "satori-token"
    },
    xiaozhi: { endpoint: "" }
  };
  const store = new MemoryStore();
  const wechatApi = {
    async createBindingQr() {
      return null;
    }
  };
  const service = new VoiceLifeService({ store, wechatApi });
  const imApplication = createImApplication({
    bindingService: createBindingServicePort(service),
    reminderCommandPort: createReminderCommandPort(service)
  });

  app.plugin(HTTP);
  app.plugin(server, { port: 0, selfUrl: config.koishi.selfUrl });
  app.plugin(createVoiceLifeKoishiPlugin({
    config,
    service,
    imApplication,
    wechatApi,
    logger: { info() {} }
  }));
  app.plugin(satoriServer, {
    path: config.koishi.satoriPath,
    token: config.koishi.satoriToken,
    webhooks: []
  });

  await app.start();
  const paths = app.server.stack.map((layer) => String(layer.path));
  assert.ok(paths.includes("/wechat-official"));
  assert.ok(paths.includes("/voicelife/reminder-actions/:token"));
  assert.ok(paths.includes("/satori/v1/:name"));

  const actionMatch = app.server.match(
    "/voicelife/reminder-actions/payload.signature",
    "GET"
  );
  assert.equal(actionMatch.route, true);
  const actionLayer = actionMatch.pathAndMethod.find(
    (layer) => layer.path === "/voicelife/reminder-actions/:token"
  );
  assert.ok(actionLayer);
  assert.equal(
    actionLayer.params(
      "/voicelife/reminder-actions/payload.signature",
      actionLayer.captures(
        "/voicelife/reminder-actions/payload.signature"
      )
    ).token,
    "payload.signature"
  );

  await app.stop();
});
