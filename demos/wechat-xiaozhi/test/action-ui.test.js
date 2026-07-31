import assert from "node:assert/strict";
import test from "node:test";
import {
  actionPage,
  reminderActionPath
} from "../src/action-ui.js";

test("Action UI 把签名令牌放在 VoiceLife 产品路由中", () => {
  const token = "payload.signature";
  const html = actionPage({
    reminder: {
      title: "喝水",
      dueAt: "2026-07-31T10:00:00.000Z"
    },
    token
  });

  assert.equal(
    reminderActionPath(token),
    "/voicelife/reminder-actions/payload.signature"
  );
  assert.match(
    html,
    /action="\/voicelife\/reminder-actions\/payload\.signature"/
  );
  assert.doesNotMatch(html, /name="token"/);
});
