import assert from "node:assert/strict";
import test from "node:test";
import { createBindingHandler } from "../src/binding-handler.js";
import { createImApplication } from "../src/im-application.js";

test("Binding Handler 只把规范化身份交给 Binding Service", () => {
  const calls = [];
  const application = createImApplication({
    bindingService: {
      bind(request) {
        calls.push(request);
        return { deviceId: "xiaozhi-01" };
      },
      deactivate() {
        return true;
      }
    },
    reminderCommandPort: {
      inspect() {},
      execute() {}
    }
  });
  const handler = createBindingHandler({
    bindingApplication: application.binding
  });
  const event = {
    type: "binding.requested",
    eventId: "event-01",
    channelAccountId: "gh-demo",
    actor: {
      platform: "wechat-official",
      userId: "openid-01"
    },
    pairingCode: "ABC123"
  };

  assert.equal(handler.handle(event).binding.deviceId, "xiaozhi-01");
  assert.equal(handler.handle(event).binding.deviceId, "xiaozhi-01");
  assert.deepEqual(calls, [{
    pairingCode: "ABC123",
    actor: {
      platform: "wechat-official",
      userId: "openid-01"
    }
  }]);
});

test("IM Application.Action 只通过 Reminder Command Port 执行业务动作", () => {
  const commands = [];
  const application = createImApplication({
    bindingService: {
      bind() {},
      deactivate() {}
    },
    reminderCommandPort: {
      inspect(intent) {
        return intent;
      },
      execute(command) {
        commands.push(command);
        return { status: "dismissed" };
      }
    }
  });

  assert.deepEqual(
    application.action.execute({ reminderId: "reminder-01", action: "dismiss" }),
    { status: "dismissed" }
  );
  assert.deepEqual(commands, [{
    reminderId: "reminder-01",
    action: "dismiss"
  }]);
});
