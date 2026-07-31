import fs from "node:fs";
import path from "node:path";

function emptyState() {
  return {
    bindCodes: {},
    bindings: {},
    reminders: {},
    outbound: [],
    receipts: []
  };
}

function migrateState(state) {
  for (const binding of Object.values(state.bindings)) {
    if (!binding.externalIdentity && binding.openId) {
      binding.externalIdentity = {
        platform: "wechat-official",
        userId: binding.openId
      };
      delete binding.openId;
    }
  }
  return state;
}

export class JsonStore {
  constructor(filePath) {
    this.filePath = filePath;
    this.state = emptyState();
    this.load();
  }

  load() {
    try {
      this.state = migrateState({
        ...emptyState(),
        ...JSON.parse(fs.readFileSync(this.filePath, "utf8"))
      });
    } catch (error) {
      if (error.code !== "ENOENT") throw error;
    }
  }

  save() {
    fs.mkdirSync(path.dirname(this.filePath), { recursive: true });
    const temporary = `${this.filePath}.tmp`;
    fs.writeFileSync(temporary, `${JSON.stringify(this.state, null, 2)}\n`, {
      mode: 0o600
    });
    fs.renameSync(temporary, this.filePath);
  }

  snapshot() {
    return structuredClone(this.state);
  }

  mutate(mutator) {
    const result = mutator(this.state);
    this.save();
    return result;
  }
}

export class MemoryStore {
  constructor(initial = {}) {
    this.state = migrateState({ ...emptyState(), ...structuredClone(initial) });
  }

  snapshot() {
    return structuredClone(this.state);
  }

  mutate(mutator) {
    return mutator(this.state);
  }
}
