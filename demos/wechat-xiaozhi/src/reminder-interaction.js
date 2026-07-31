import { verifyReminderActionToken } from "./action-token.js";

const BUTTON_PREFIX = "voicelife.reminder";
const ACTIONS = new Set(["dismiss", "snooze"]);

function assertAction(action) {
  if (!ACTIONS.has(action)) throw new Error("未知操作");
  return action;
}

export function createReminderButtonId({ action, token }) {
  return `${BUTTON_PREFIX}:${assertAction(action)}:${token}`;
}

export function parseReminderButtonId(value) {
  const [prefix, action, ...tokenParts] = String(value || "").split(":");
  if (prefix !== BUTTON_PREFIX || !ACTIONS.has(action)) return null;
  const token = tokenParts.join(":");
  return token ? { action, token } : null;
}

export function createReminderInteractionHandler({
  service,
  tokenSecret,
  now = () => Date.now()
}) {
  function verify(token) {
    return verifyReminderActionToken(token, tokenSecret, now());
  }

  return {
    inspect(token) {
      return service.findActionIntentTarget(verify(token));
    },

    execute({ token, action }) {
      const command = {
        ...verify(token),
        action: assertAction(action)
      };
      if (action === "snooze") command.minutes = 10;
      const reminder = service.executeReminderAction(command);
      return action === "dismiss"
        ? {
            action,
            reminder,
            title: "已知道",
            detail: "这条提醒已经完成",
            message: "✅ 已知道"
          }
        : {
            action,
            reminder,
            title: "已推迟 10 分钟",
            detail: `将在 ${reminder.dueAt} 再次提醒`,
            message: "⏰ 已推迟 10 分钟"
          };
    }
  };
}

export async function updateReminderInteractionMessage(session, result) {
  if (
    session.channelId &&
    session.messageId &&
    typeof session.bot?.editMessage === "function"
  ) {
    try {
      await session.bot.editMessage(
        session.channelId,
        session.messageId,
        result.message
      );
      return "edited";
    } catch (error) {
      if (typeof session.send !== "function") throw error;
    }
  }
  if (typeof session.send === "function") {
    await session.send(result.message);
    return "replied";
  }
  return "skipped";
}

export async function handleReminderButtonInteraction(
  session,
  interaction,
  { updateMessage = updateReminderInteractionMessage } = {}
) {
  const input = parseReminderButtonId(session.event?.button?.id);
  if (!input) return false;
  const result = interaction.execute(input);
  await updateMessage(session, result);
  return true;
}

export function registerReminderInteractionHandler(ctx, interaction) {
  return ctx.on("interaction/button", async (session) => {
    try {
      await handleReminderButtonInteraction(session, interaction);
    } catch (error) {
      if (typeof session.send === "function") {
        await session.send(`操作失败：${error.message}`);
      }
    }
  });
}
