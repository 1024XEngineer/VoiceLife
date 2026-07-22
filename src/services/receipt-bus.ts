import { EventEmitter } from "node:events";
import type { Receipt } from "../domain/types.js";

export class ReceiptBus extends EventEmitter {
  publish(receipt: Receipt): void {
    this.emit("receipt", receipt);
  }

  subscribe(listener: (receipt: Receipt) => void): () => void {
    this.on("receipt", listener);
    return () => this.off("receipt", listener);
  }
}
