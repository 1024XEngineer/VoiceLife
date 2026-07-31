export function createBindingServicePort(service) {
  return {
    bind({ pairingCode, actor }) {
      return service.bindExternalIdentity(pairingCode, actor);
    },

    deactivate({ actor }) {
      return service.deactivateExternalIdentity(actor);
    }
  };
}

export function createReminderCommandPort(service) {
  return {
    inspect(intent) {
      return service.findActionIntentTarget(intent);
    },

    execute(command) {
      return service.executeReminderAction(command);
    }
  };
}
