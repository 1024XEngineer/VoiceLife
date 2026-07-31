const BINDING_REQUESTED = "binding.requested";
const BINDING_DEACTIVATED = "binding.deactivated";

function assertActor(actor) {
  if (!actor?.platform || !actor?.userId) {
    throw new Error("绑定事件缺少 ExternalIdentity");
  }
  return {
    platform: String(actor.platform),
    userId: String(actor.userId)
  };
}

export function createBindingHandler({ bindingApplication }) {
  const handled = new Map();

  return {
    handle(event) {
      if (!event?.eventId) throw new Error("绑定事件缺少 eventId");
      if (!event?.channelAccountId) throw new Error("绑定事件缺少 channelAccountId");
      const idempotencyKey = `${event.channelAccountId}:${event.eventId}`;
      if (handled.has(idempotencyKey)) return handled.get(idempotencyKey);

      const actor = assertActor(event.actor);
      let result;
      if (event.type === BINDING_REQUESTED) {
        if (!event.pairingCode) throw new Error("绑定事件缺少 pairingCode");
        result = {
          status: "bound",
          binding: bindingApplication.bind({
            pairingCode: event.pairingCode,
            actor
          })
        };
      } else if (event.type === BINDING_DEACTIVATED) {
        result = {
          status: bindingApplication.deactivate({ actor })
            ? "deactivated"
            : "not_found"
        };
      } else {
        throw new Error(`不支持的绑定事件：${event.type || "unknown"}`);
      }

      handled.set(idempotencyKey, result);
      return result;
    }
  };
}

export const BindingEventType = Object.freeze({
  REQUESTED: BINDING_REQUESTED,
  DEACTIVATED: BINDING_DEACTIVATED
});
