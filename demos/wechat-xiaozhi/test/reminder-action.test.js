import assert from "node:assert/strict";
import test from "node:test";
import { createReminderActionToken } from "../src/action-token.js";
import {
  createReminderButtonId,
  createReminderActionHandler,
  handleReminderButtonAction,
  updateReminderActionMessage
} from "../src/reminder-action.js";

function fixture() {
  const reminder = {
    id: "reminder-01",
    dueAt: "2026-07-31T10:00:00.000Z",
    status: "sent"
  };
  const commands = [];
  const actionApplication = {
    inspect(input) {
      assert.equal(input.reminderId, reminder.id);
      return reminder;
    },
    execute(command) {
      commands.push(command);
      return command.action === "snooze"
        ? { ...reminder, status: "scheduled", dueAt: "2026-07-31T10:10:00.000Z" }
        : { ...reminder, status: "dismissed" };
    }
  };
  const token = createReminderActionToken({
    reminder,
    secret: "action-secret",
    now: 1_000,
    ttlSeconds: 600
  });
  const actionHandler = createReminderActionHandler({
    actionApplication,
    tokenSecret: "action-secret",
    now: () => 1_000
  });
  return { actionHandler, commands, reminder, token };
}

test("H5 与原生卡片复用同一个提醒动作处理器", async () => {
  const { actionHandler, commands, token } = fixture();

  actionHandler.execute({ token, action: "dismiss" });
  const session = {
    event: {
      button: {
        id: createReminderButtonId({ action: "snooze", token })
      }
    }
  };
  let cardResult;
  const handled = await handleReminderButtonAction(
    session,
    actionHandler,
    {
      async updateMessage(_session, result) {
        cardResult = result;
      }
    }
  );

  assert.equal(handled, true);
  assert.deepEqual(
    commands.map(({ action, minutes }) => ({ action, minutes })),
    [
      { action: "dismiss", minutes: undefined },
      { action: "snooze", minutes: 10 }
    ]
  );
  assert.equal(cardResult.message, "⏰ 已推迟 10 分钟");
});

test("卡片更新优先使用 Koishi bot.editMessage", async () => {
  const calls = [];
  const session = {
    channelId: "channel-01",
    messageId: "message-01",
    bot: {
      async editMessage(...args) {
        calls.push(args);
      }
    },
    async send() {
      throw new Error("不应降级为发送新消息");
    }
  };

  const mode = await updateReminderActionMessage(session, {
    message: "✅ 已知道"
  });

  assert.equal(mode, "edited");
  assert.deepEqual(calls, [["channel-01", "message-01", "✅ 已知道"]]);
});

test("平台不支持修改消息时降级为 Koishi session.send", async () => {
  const replies = [];
  const session = {
    channelId: "channel-01",
    messageId: "message-01",
    bot: {
      async editMessage() {
        throw new Error("unsupported");
      }
    },
    async send(content) {
      replies.push(content);
    }
  };

  const mode = await updateReminderActionMessage(session, {
    message: "✅ 已知道"
  });

  assert.equal(mode, "replied");
  assert.deepEqual(replies, ["✅ 已知道"]);
});
