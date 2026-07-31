import assert from "node:assert/strict";
import test from "node:test";
import { createBindingHandler } from "../src/binding-handler.js";
import { createImApplication } from "../src/im-application.js";
import {
  deactivateWechatBinding,
  handleWechatBindingEvent,
  handleWechatBindingText
} from "../src/wechat-domain.js";
import { VoiceLifeService } from "../src/service.js";
import { MemoryStore } from "../src/store.js";
import {
  createBindingServicePort,
  createReminderCommandPort
} from "../src/voicelife-ports.js";

function fixture() {
  const store = new MemoryStore();
  const service = new VoiceLifeService({
    store,
    wechatApi: {},
    now: () => Date.parse("2026-07-29T10:00:00+08:00")
  });
  const application = createImApplication({
    bindingService: createBindingServicePort(service),
    reminderCommandPort: createReminderCommandPort(service)
  });
  const bindingHandler = createBindingHandler({
    bindingApplication: application.binding
  });
  return { bindingHandler, store, service };
}

test("Koishi 标准文本会话复用 VoiceLife 绑定逻辑", () => {
  const { bindingHandler, service } = fixture();
  const code = service.createBindingCode("xiaozhi-01");
  assert.equal(
    handleWechatBindingText({
      content: `绑定 ${code.code}`,
      eventId: "message-01",
      openId: "openid-01",
      bindingHandler
    }),
    "绑定成功：xiaozhi-01"
  );
});

test("微信文字不再执行提醒动作", () => {
  const { bindingHandler } = fixture();
  for (const content of ["知道了", "关闭", "推迟10分钟"]) {
    assert.match(
      handleWechatBindingText({
        content,
        eventId: `message-${content}`,
        openId: "openid-01",
        bindingHandler
      }),
      /绑定/
    );
  }
});

test("微信专属扫码事件和取消关注仍由边界层处理", () => {
  const { bindingHandler, store, service } = fixture();
  const code = service.createBindingCode("xiaozhi-01");
  assert.equal(
    handleWechatBindingEvent({
      eventKey: `qrscene_${code.code}`,
      eventId: "scan-01",
      openId: "openid-01",
      bindingHandler
    }),
    "扫码绑定成功：xiaozhi-01"
  );
  assert.equal(deactivateWechatBinding({
    eventId: "unsubscribe-01",
    openId: "openid-01",
    bindingHandler
  }), true);
  assert.equal(store.state.bindings["xiaozhi-01"].active, false);
});
