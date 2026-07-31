function requirePort(port, name) {
  if (!port || typeof port !== "object") {
    throw new Error(`${name} 未配置`);
  }
  return port;
}

export function createImApplication({ bindingService, reminderCommandPort }) {
  const binding = requirePort(bindingService, "Binding Service");
  const reminder = requirePort(reminderCommandPort, "Reminder Command Port");

  return {
    binding: {
      bind(request) {
        return binding.bind(request);
      },

      deactivate(request) {
        return binding.deactivate(request);
      }
    },

    action: {
      inspect(intent) {
        return reminder.inspect(intent);
      },

      execute(command) {
        return reminder.execute(command);
      }
    }
  };
}
