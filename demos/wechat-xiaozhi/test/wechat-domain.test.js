import assert from "node:assert/strict";
import test from "node:test";
import {
  deactivateWechatBinding,
  handleWechatBindingEvent,
  handleWechatText
} from "../src/wechat-domain.js";
import { VoiceLifeService } from "../src/service.js";
import { MemoryStore } from "../src/store.js";

function fixture() {
  const store = new MemoryStore();
  const service = new VoiceLifeService({
    store,
    wechatApi: {},
    now: () => Date.parse("2026-07-29T10:00:00+08:00")
  });
  return { store, service };
}

test("Koishi 标准文本会话复用 VoiceLife 绑定逻辑", () => {
  const { service } = fixture();
  const code = service.createBindingCode("xiaozhi-01");
  assert.equal(
    handleWechatText({
      content: `绑定 ${code.code}`,
      openId: "openid-01",
      service
    }),
    "绑定成功：xiaozhi-01"
  );
});

test("微信专属扫码事件和取消关注仍由边界层处理", () => {
  const { store, service } = fixture();
  const code = service.createBindingCode("xiaozhi-01");
  assert.equal(
    handleWechatBindingEvent({
      eventKey: `qrscene_${code.code}`,
      openId: "openid-01",
      service
    }),
    "扫码绑定成功：xiaozhi-01"
  );
  assert.equal(deactivateWechatBinding({ openId: "openid-01", service }), true);
  assert.equal(store.state.bindings["xiaozhi-01"].active, false);
});
