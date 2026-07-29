import assert from "node:assert/strict";
import test from "node:test";
import {
  createReminderActionToken,
  verifyReminderActionToken
} from "../src/action-token.js";

const reminder = {
  id: "reminder-01",
  dueAt: "2026-07-29T02:01:00.000Z"
};

test("提醒操作令牌可验证提醒版本和有效期", () => {
  const token = createReminderActionToken({
    reminder,
    secret: "test-secret",
    now: 1_000,
    ttlSeconds: 60
  });
  assert.deepEqual(
    verifyReminderActionToken(token, "test-secret", 30_000),
    {
      reminderId: reminder.id,
      dueAt: reminder.dueAt,
      exp: 61
    }
  );
  assert.throws(
    () => verifyReminderActionToken(token, "test-secret", 62_000),
    /已过期/
  );
});

test("被篡改的提醒操作令牌会被拒绝", () => {
  const token = createReminderActionToken({
    reminder,
    secret: "test-secret",
    now: 1_000
  });
  assert.throws(
    () => verifyReminderActionToken(`${token}x`, "test-secret", 2_000),
    /签名无效/
  );
});
