import { randomUUID } from "node:crypto";
import type { Receipt, ShortNote } from "../domain/types.js";
import { toStorageIso } from "../domain/recurrence.js";
import { CalendarDatabase } from "../storage/database.js";
import type { Clock } from "./clock.js";
import { ReceiptBus } from "./receipt-bus.js";

const SENSITIVE_PATTERN = /密码|口令|验证码|取件码|支付码|pin\b|password|secret/i;

export class ShortNoteService {
  public constructor(
    private readonly db: CalendarDatabase,
    private readonly clock: Clock,
    private readonly receiptBus: ReceiptBus,
  ) {}

  record(input: { content: string; category?: string }): { note: ShortNote; receipt: Receipt } {
    const content = input.content.trim();
    if (!content) throw new Error("记录内容不能为空");
    if (SENSITIVE_PATTERN.test(content)) {
      throw new Error("当前只记录非敏感小事，不能保存密码、验证码或取件码");
    }
    const now = this.clock.now();
    const note: ShortNote = {
      id: randomUUID(),
      content,
      category: input.category?.trim() || null,
      createdAt: toStorageIso(now),
      expiresAt: toStorageIso(now.plus({ hours: 24 })),
    };
    const receipt: Receipt = {
      id: randomUUID(),
      type: "note_recorded",
      eventId: null,
      reminderId: null,
      title: "已记录",
      body: content,
      data: { noteId: note.id, category: note.category, expiresAt: note.expiresAt },
      createdAt: note.createdAt,
    };
    this.db.transaction(() => {
      this.db.insertShortNote(note);
      this.db.insertReceipt(receipt);
    });
    this.receiptBus.publish(receipt);
    return { note, receipt };
  }

  query(query?: string): { notes: ShortNote[]; receipt: Receipt } {
    const now = toStorageIso(this.clock.now());
    const notes = this.db.listActiveShortNotes(now, query?.trim() || undefined);
    const receipt: Receipt = {
      id: randomUUID(),
      type: "note_query",
      eventId: null,
      reminderId: null,
      title: "临时记录查询",
      body: notes.length ? notes.map((note) => note.content).join("\n") : "没有找到仍在有效期内的记录",
      data: { query: query?.trim() || null, notes },
      createdAt: now,
    };
    this.db.insertReceipt(receipt);
    this.receiptBus.publish(receipt);
    return { notes, receipt };
  }
}
