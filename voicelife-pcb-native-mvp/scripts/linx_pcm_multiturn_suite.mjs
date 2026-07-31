#!/usr/bin/env node

import { execFile } from "node:child_process";
import { createHash } from "node:crypto";
import { mkdtemp, readFile, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import path from "node:path";
import { createRequire } from "node:module";
import { promisify } from "node:util";
import { tools } from "./linx_pcm_protocol_tools.mjs";

const require = createRequire(new URL("../../XE6-15-pr62-fresh/package.json", import.meta.url));
const { WebSocket } = require("ws");
const execFileAsync = promisify(execFile);

const requiredEnv = ["LINX_DEVICE_ID", "LINX_DEVICE_CLIENT_ID", "LINX_AGENT_ID"];
for (const name of requiredEnv) {
  if (!process.env[name]?.trim()) throw new Error(`${name} is required`);
}

const config = {
  otaUrl: process.env.LINX_DEVICE_OTA_URL?.trim() || "https://xrobo.qiniuapi.com/v1/ota/",
  deviceId: process.env.LINX_DEVICE_ID.trim(),
  clientId: process.env.LINX_DEVICE_CLIENT_ID.trim(),
  agentId: process.env.LINX_AGENT_ID.trim(),
  voice: process.env.VOICE_TEST_VOICE?.trim() || "Tingting",
  timeoutMs: Number(process.env.VOICE_TEST_TIMEOUT_MS || 150_000),
  turnGapMs: Number(process.env.VOICE_TEST_TURN_GAP_MS || 500),
};

function shanghaiDateAtOffset(dayOffset) {
  const parts = Object.fromEntries(new Intl.DateTimeFormat("en-CA", {
    timeZone: "Asia/Shanghai",
    year: "numeric",
    month: "2-digit",
    day: "2-digit",
  }).formatToParts(new Date()).filter((part) => part.type !== "literal")
    .map((part) => [part.type, part.value]));
  const date = new Date(Date.UTC(Number(parts.year), Number(parts.month) - 1, Number(parts.day) + dayOffset));
  return date.toISOString().slice(0, 10);
}

function nextWeekdayDate(weekday) {
  for (let offset = 1; offset <= 7; offset += 1) {
    const date = shanghaiDateAtOffset(offset);
    if (new Date(`${date}T00:00:00Z`).getUTCDay() === weekday) return date;
  }
  throw new Error(`could not resolve weekday ${weekday}`);
}

const today = shanghaiDateAtOffset(0);
const tomorrow = shanghaiDateAtOffset(1);
const dayAfterTomorrow = shanghaiDateAtOffset(2);
const threeDaysLater = shanghaiDateAtOffset(3);
const nextMonday = nextWeekdayDate(1);
const at = (date, time) => `${date}T${time}+08:00`;
const tomorrowStart = at(tomorrow, "00:00:00");
const tomorrowEnd = at(dayAfterTomorrow, "00:00:00");
const dayAfterTomorrowStart = at(dayAfterTomorrow, "00:00:00");
const dayAfterTomorrowEnd = at(threeDaysLater, "00:00:00");
const tomorrowNine = at(tomorrow, "09:00:00");
const tomorrowTen = at(tomorrow, "10:00:00");
const tomorrowFourteen = at(tomorrow, "14:00:00");
const tomorrowFifteen = at(tomorrow, "15:00:00");
const tomorrowFifteenOneMinute = at(tomorrow, "15:01:00");
const tomorrowSixteen = at(tomorrow, "16:00:00");
const nextMondayNine = at(nextMonday, "09:00:00");
const nextMondayTen = at(nextMonday, "10:00:00");
const nextFriday = nextWeekdayDate(5);
const nextFridaySeventeen = at(nextFriday, "17:00:00");
const todayEighteen = at(today, "18:00:00");
const todayNineteen = at(today, "19:00:00");
const todayNineteenOneMinute = at(today, "19:01:00");
const todayNineteenThirty = at(today, "19:30:00");
const todayNineteenThirtyOneMinute = at(today, "19:31:00");
const todayTwenty = at(today, "20:00:00");

function sameInstant(actual, expected) {
  return typeof actual === "string" && Date.parse(actual) === Date.parse(expected);
}

function alignWeeklyStartAt(proposedStartAt, weekday) {
  const proposed = new Date(proposedStartAt);
  if (!Number.isFinite(proposed.getTime()) || !Number.isInteger(weekday) || weekday < 1 || weekday > 7) {
    return null;
  }
  const clock = Object.fromEntries(new Intl.DateTimeFormat("en-GB", {
    timeZone: "Asia/Shanghai",
    hour: "2-digit",
    minute: "2-digit",
    second: "2-digit",
    hourCycle: "h23",
  }).formatToParts(proposed).filter((part) => part.type !== "literal")
    .map((part) => [part.type, part.value]));
  const time = `${clock.hour}:${clock.minute}:${clock.second}`;
  for (let offset = 0; offset <= 7; offset += 1) {
    const date = shanghaiDateAtOffset(offset);
    if (new Date(`${date}T00:00:00Z`).getUTCDay() !== weekday % 7) continue;
    const candidate = at(date, time);
    if (Date.parse(candidate) >= Date.now() - 1_000) return candidate;
  }
  return null;
}

function callsByName(observed, name) {
  return observed.toolCalls.filter((call) => call.name === name);
}

function firstCall(observed, name) {
  return callsByName(observed, name)[0];
}

function firstResult(observed, name) {
  return observed.toolResults.find((item) => item.name === name)?.result;
}

function noInternalNarration(text) {
  return !/(当前时间|今天是\d|明天是\d|用户说|我先查|我来查|calendar_|reminder_|note_|rangeStart|rangeEnd|startsAt|durationMinutes|\bkind\b|JSON|工具名|参数说明|选择理由|思考过程)/iu.test(text);
}

function eventFixture({ id, title, startsAt, kind = "point", recurrence = null }) {
  return {
    eventId: id,
    title,
    startsAt,
    originalStartAt: startsAt,
    kind,
    location: "",
    notes: "",
    recurrence,
  };
}

function reminderFixture({ id, title, dueAt, eventId = "event-fixture" }) {
  return {
    reminderId: id,
    eventId,
    title,
    dueAt,
    originalStartAt: dueAt,
    weak: false,
    snoozeCount: 0,
  };
}

function unexpected(call) {
  return { ok: false, message: `Unexpected tool ${call.name}` };
}

function writeResult(speech, extra = {}) {
  return { ok: true, speech, ...extra };
}

function strictWriteAssertions(observed, toolName) {
  const speech = firstResult(observed, toolName)?.speech;
  return {
    ttsExactlySpeech: Boolean(speech) && observed.spokenText === speech,
    noInternalNarration: noInternalNarration(observed.spokenText),
  };
}

const scenarios = [
  {
    id: "conflict_confirm_create",
    createState: () => ({ initial: null, token: "create-conflict-token" }),
    turns: [
      {
        id: "request_conflicting_reminder",
        phrase: "明天上午九点提醒我参加冲突会议",
        respond(call, state) {
          if (call.name !== "calendar_create") return unexpected(call);
          state.initial = structuredClone(call.arguments);
          return {
            ok: false,
            message: "时间与已有日程冲突",
            speech: "时间与已有日程冲突",
            reason: "calendar_conflict",
            requiresConfirmation: true,
            confirmationToken: state.token,
            conflictConfirmationToken: state.token,
            conflicts: [eventFixture({ id: "existing-conflict", title: "已有会议", startsAt: tomorrowNine })],
          };
        },
        assert(observed) {
          const create = firstCall(observed, "calendar_create");
          return {
            functional: {
              onlyCreateCalled: observed.toolCalls.length === 1 && Boolean(create),
              createsPointAtNine: create?.arguments.kind === "point"
                && sameInstant(create?.arguments.startsAt, tomorrowNine),
              noTokenOnFirstAttempt: !create?.arguments.conflictConfirmationToken,
              asksForConflictConfirmation: observed.spokenText === "时间与已有日程冲突，是否仍要创建？",
              doesNotClaimSuccess: !observed.spokenText.includes("已创建"),
            },
            quality: { noInternalNarration: noInternalNarration(observed.spokenText) },
          };
        },
      },
      {
        id: "confirm_conflicting_reminder",
        phrase: "仍然创建",
        respond(call, state) {
          if (call.name !== "calendar_create") return unexpected(call);
          if (call.arguments.conflictConfirmationToken !== state.token) {
            return { ok: false, message: "冲突确认令牌无效" };
          }
          return writeResult(`已创建参加冲突会议，时间是${tomorrowNine}`, {
            eventId: "created-conflict",
            reminderId: "created-conflict-reminder",
            undoOperationId: "undo-created-conflict",
          });
        },
        assert(observed, state) {
          const create = firstCall(observed, "calendar_create");
          return {
            functional: {
              onlyCreateCalled: observed.toolCalls.length === 1 && Boolean(create),
              reusesConflictToken: create?.arguments.conflictConfirmationToken === state.token,
              preservesBusinessPayload: create?.arguments.title === state.initial?.title
                && create?.arguments.kind === state.initial?.kind
                && sameInstant(create?.arguments.startsAt, state.initial?.startsAt),
            },
            quality: strictWriteAssertions(observed, "calendar_create"),
          };
        },
      },
    ],
  },
  {
    id: "delete_confirm",
    createState: () => ({ token: "delete-confirm-token" }),
    turns: [
      {
        id: "request_delete",
        phrase: "删除明天上午九点的开会",
        respond(call, state) {
          if (call.name === "calendar_find") {
            return {
              ok: true,
              speech: "找到一条日程。",
              total: 1,
              requiresDisambiguation: false,
              candidates: [eventFixture({ id: "event-delete", title: "开会", startsAt: tomorrowNine })],
            };
          }
          if (call.name === "calendar_delete") {
            if (call.arguments.confirmationToken === state.token) {
              return writeResult("已删除开会。", { undoOperationId: "undo-delete-confirm" });
            }
            return {
              ok: false,
              message: "删除后可在十分钟内撤销，确认删除吗？",
              speech: "删除后可在十分钟内撤销，确认删除吗？",
              requiresConfirmation: true,
              confirmationToken: state.token,
            };
          }
          return unexpected(call);
        },
        assert(observed) {
          const deletion = firstCall(observed, "calendar_delete");
          return {
            functional: {
              findThenDelete: observed.toolCalls.length === 2
                && observed.toolCalls[0].name === "calendar_find"
                && observed.toolCalls[1].name === "calendar_delete",
              targetsReturnedEvent: deletion?.arguments.eventId === "event-delete",
              noTokenOnFirstDelete: !deletion?.arguments.confirmationToken,
              asksBeforeDelete: /(确认|是否).*(删除)|删除.*[吗？?]/u.test(observed.spokenText),
              doesNotClaimDeleted: !observed.spokenText.includes("已删除"),
            },
            quality: { noInternalNarration: noInternalNarration(observed.spokenText) },
          };
        },
      },
      {
        id: "confirm_delete",
        phrase: "确认删除",
        respond(call, state) {
          if (call.name !== "calendar_delete") return unexpected(call);
          if (call.arguments.confirmationToken !== state.token) {
            return { ok: false, message: "删除确认令牌无效" };
          }
          return writeResult("已删除开会。", { undoOperationId: "undo-delete-confirm" });
        },
        assert(observed, state) {
          const deletion = firstCall(observed, "calendar_delete");
          return {
            functional: {
              onlyDeleteCalled: observed.toolCalls.length === 1 && Boolean(deletion),
              reusesEventAndToken: deletion?.arguments.eventId === "event-delete"
                && deletion?.arguments.confirmationToken === state.token,
            },
            quality: strictWriteAssertions(observed, "calendar_delete"),
          };
        },
      },
    ],
  },
  {
    id: "delete_cancel",
    createState: () => ({ token: "delete-cancel-token" }),
    turns: [
      {
        id: "request_delete",
        phrase: "删除明天上午九点的体检",
        respond(call, state) {
          if (call.name === "calendar_find") {
            return {
              ok: true,
              total: 1,
              requiresDisambiguation: false,
              candidates: [eventFixture({ id: "event-cancel-delete", title: "体检", startsAt: tomorrowNine })],
            };
          }
          if (call.name === "calendar_delete") {
            return {
              ok: false,
              message: "确认删除体检吗？",
              speech: "确认删除体检吗？",
              requiresConfirmation: true,
              confirmationToken: state.token,
            };
          }
          return unexpected(call);
        },
        assert(observed) {
          return {
            functional: {
              findThenDelete: observed.toolCalls.map((call) => call.name).join(",") === "calendar_find,calendar_delete",
              asksBeforeDelete: /(确认|是否).*(删除)|删除.*[吗？?]/u.test(observed.spokenText),
            },
            quality: { noInternalNarration: noInternalNarration(observed.spokenText) },
          };
        },
      },
      {
        id: "cancel_delete",
        phrase: "算了，不删了",
        respond: unexpected,
        assert(observed) {
          return {
            functional: {
              noToolCalled: observed.toolCalls.length === 0,
              acknowledgesCancellation: /(好|取消|不删|保留|明白)/u.test(observed.spokenText),
              doesNotClaimDeleted: !observed.spokenText.includes("已删除"),
            },
            quality: { noInternalNarration: noInternalNarration(observed.spokenText) },
          };
        },
      },
    ],
  },
  {
    id: "create_then_undo",
    createState: () => ({ undoId: "undo-create-report" }),
    turns: [
      {
        id: "create_reminder",
        phrase: "明天下午两点提醒我交报告",
        respond(call, state) {
          if (call.name !== "calendar_create") return unexpected(call);
          return writeResult(`已创建交报告，时间是${tomorrowFourteen}`, {
            eventId: "event-report",
            reminderId: "reminder-report",
            undoOperationId: state.undoId,
          });
        },
        assert(observed) {
          const create = firstCall(observed, "calendar_create");
          return {
            functional: {
              onlyCreateCalled: observed.toolCalls.length === 1 && Boolean(create),
              createsPointAtTwo: create?.arguments.kind === "point"
                && sameInstant(create?.arguments.startsAt, tomorrowFourteen),
            },
            quality: strictWriteAssertions(observed, "calendar_create"),
          };
        },
      },
      {
        id: "undo_create",
        phrase: "撤销刚才操作",
        respond(call, state) {
          if (call.name !== "calendar_undo") return unexpected(call);
          if (call.arguments.undoOperationId !== state.undoId) return { ok: false, message: "撤销令牌无效" };
          return writeResult("已撤销刚才的创建操作。");
        },
        assert(observed, state) {
          const undo = firstCall(observed, "calendar_undo");
          return {
            functional: {
              onlyUndoCalled: observed.toolCalls.length === 1 && Boolean(undo),
              reusesUndoId: undo?.arguments.undoOperationId === state.undoId,
            },
            quality: strictWriteAssertions(observed, "calendar_undo"),
          };
        },
      },
    ],
  },
  {
    id: "modify_after_disambiguation",
    turns: [
      {
        id: "request_ambiguous_modify",
        phrase: "把明天的项目会改到下午四点",
        respond(call) {
          if (call.name !== "calendar_find") return unexpected(call);
          return {
            ok: true,
            speech: "找到两条项目会：上午九点和下午两点，请选择一条。",
            total: 2,
            requiresDisambiguation: true,
            candidates: [
              eventFixture({ id: "project-am", title: "项目会", startsAt: tomorrowNine }),
              eventFixture({ id: "project-pm", title: "项目会", startsAt: tomorrowFourteen }),
            ],
          };
        },
        assert(observed) {
          return {
            functional: {
              onlyFindCalled: observed.toolCalls.length === 1 && observed.toolCalls[0].name === "calendar_find",
              offersBothCandidates: /(九点|9点)/u.test(observed.spokenText) && /(两点|2点)/u.test(observed.spokenText),
              asksUserToChoose: /(哪|选择|一个|那个)/u.test(observed.spokenText),
            },
            quality: { noInternalNarration: noInternalNarration(observed.spokenText) },
          };
        },
      },
      {
        id: "choose_candidate",
        phrase: "上午九点那个",
        respond(call) {
          if (call.name === "calendar_find") {
            return {
              ok: true,
              total: 1,
              requiresDisambiguation: false,
              candidates: [eventFixture({ id: "project-am", title: "项目会", startsAt: tomorrowNine })],
            };
          }
          if (call.name === "calendar_modify") {
            return writeResult(`已修改项目会，时间是${tomorrowSixteen}`, {
              event: eventFixture({ id: "project-am", title: "项目会", startsAt: tomorrowSixteen }),
              undoOperationId: "undo-project-disambiguation",
            });
          }
          return unexpected(call);
        },
        assert(observed) {
          const modify = firstCall(observed, "calendar_modify");
          const movedAt = modify?.arguments.newStartAt || modify?.arguments.startsAt;
          return {
            functional: {
              modifiesAfterSelection: Boolean(modify),
              noWrongTool: observed.toolCalls.every((call) => ["calendar_find", "calendar_modify"].includes(call.name)),
              selectsMorningEvent: modify?.arguments.eventId === "project-am",
              preservesRequestedNewTime: sameInstant(movedAt, tomorrowSixteen),
            },
            quality: strictWriteAssertions(observed, "calendar_modify"),
          };
        },
      },
    ],
  },
  {
    id: "reminder_close_unique",
    turns: [
      {
        id: "list_due",
        phrase: "我有哪些到期提醒",
        respond(call) {
          if (call.name !== "reminder_list_due") return unexpected(call);
          return {
            ok: true,
            speech: "有1条到期提醒：喝水。",
            total: 1,
            requiresDisambiguation: false,
            reminders: [reminderFixture({ id: "reminder-water", title: "喝水", dueAt: tomorrowNine })],
          };
        },
        assert(observed) {
          return {
            functional: {
              onlyListDueCalled: observed.toolCalls.length === 1 && observed.toolCalls[0].name === "reminder_list_due",
              mentionsWater: observed.spokenText.includes("喝水"),
            },
            quality: { noInternalNarration: noInternalNarration(observed.spokenText) },
          };
        },
      },
      {
        id: "close_unique",
        phrase: "知道了，关闭提醒",
        respond(call) {
          if (call.name !== "reminder_close") return unexpected(call);
          return writeResult("已关闭喝水提醒。");
        },
        assert(observed) {
          const close = firstCall(observed, "reminder_close");
          return {
            functional: {
              onlyCloseCalled: observed.toolCalls.length === 1 && Boolean(close),
              closesListedReminder: close?.arguments.reminderId === "reminder-water",
            },
            quality: strictWriteAssertions(observed, "reminder_close"),
          };
        },
      },
    ],
  },
  {
    id: "reminder_snooze_followup",
    turns: [
      {
        id: "list_due",
        phrase: "我有哪些到期提醒",
        respond(call) {
          if (call.name !== "reminder_list_due") return unexpected(call);
          return {
            ok: true,
            speech: "有1条到期提醒：吃药。",
            total: 1,
            requiresDisambiguation: false,
            reminders: [reminderFixture({ id: "reminder-medicine", title: "吃药", dueAt: tomorrowNine })],
          };
        },
        assert(observed) {
          return {
            functional: {
              onlyListDueCalled: observed.toolCalls.length === 1 && observed.toolCalls[0].name === "reminder_list_due",
              mentionsMedicine: observed.spokenText.includes("吃药"),
            },
            quality: { noInternalNarration: noInternalNarration(observed.spokenText) },
          };
        },
      },
      {
        id: "ask_snooze_without_minutes",
        phrase: "把吃药提醒晚点再提醒",
        respond: unexpected,
        assert(observed) {
          return {
            functional: {
              noToolCalled: observed.toolCalls.length === 0,
              asksForMinutes: /(几分钟|多少分钟|多久|推迟多久)/u.test(observed.spokenText),
              doesNotClaimSnoozed: !/(已推迟|已经推迟)/u.test(observed.spokenText),
            },
            quality: { noInternalNarration: noInternalNarration(observed.spokenText) },
          };
        },
      },
      {
        id: "provide_snooze_minutes",
        phrase: "十分钟",
        respond(call) {
          if (call.name !== "reminder_snooze") return unexpected(call);
          return writeResult("已将吃药提醒推迟十分钟。", { snoozeCount: 1 });
        },
        assert(observed) {
          const snooze = firstCall(observed, "reminder_snooze");
          return {
            functional: {
              onlySnoozeCalled: observed.toolCalls.length === 1 && Boolean(snooze),
              snoozesListedReminder: snooze?.arguments.reminderId === "reminder-medicine",
              usesTenMinutes: snooze?.arguments.minutes === 10,
            },
            quality: strictWriteAssertions(observed, "reminder_snooze"),
          };
        },
      },
    ],
  },
  {
    id: "reminder_multiple_disambiguation",
    turns: [
      {
        id: "request_close_without_target",
        phrase: "关闭我的到期提醒",
        respond(call) {
          if (call.name !== "reminder_list_due") return unexpected(call);
          return {
            ok: true,
            speech: "有两条到期提醒：喝水和提交周报，请选择一条。",
            total: 2,
            requiresDisambiguation: true,
            reminders: [
              reminderFixture({ id: "due-water", title: "喝水", dueAt: tomorrowNine }),
              reminderFixture({ id: "due-report", title: "提交周报", dueAt: tomorrowTen }),
            ],
          };
        },
        assert(observed) {
          return {
            functional: {
              onlyListDueCalled: observed.toolCalls.length === 1 && observed.toolCalls[0].name === "reminder_list_due",
              mentionsBoth: observed.spokenText.includes("喝水") && observed.spokenText.includes("周报"),
              asksUserToChoose: /(选择|哪一|哪个)/u.test(observed.spokenText),
            },
            quality: { noInternalNarration: noInternalNarration(observed.spokenText) },
          };
        },
      },
      {
        id: "choose_due_reminder",
        phrase: "喝水那个",
        respond(call) {
          if (call.name !== "reminder_close") return unexpected(call);
          return writeResult("已关闭喝水提醒。");
        },
        assert(observed) {
          const close = firstCall(observed, "reminder_close");
          return {
            functional: {
              onlyCloseCalled: observed.toolCalls.length === 1 && Boolean(close),
              closesChosenReminder: close?.arguments.reminderId === "due-water",
            },
            quality: strictWriteAssertions(observed, "reminder_close"),
          };
        },
      },
    ],
  },
  {
    id: "recurrence_lifecycle",
    turns: [
      {
        id: "create_weekly",
        phrase: "每周一上午九点提醒我开周会",
        respond(call) {
          if (call.name !== "calendar_create") return unexpected(call);
          return writeResult(`已创建每周一的周会提醒，首次时间是${nextMondayNine}`, {
            eventId: "weekly-standup",
            reminderId: "weekly-standup-reminder",
            undoOperationId: "undo-weekly-create",
          });
        },
        assert(observed) {
          const create = firstCall(observed, "calendar_create");
          return {
            functional: {
              onlyCreateCalled: observed.toolCalls.length === 1 && Boolean(create),
              weeklyRecurrence: create?.arguments.recurrence?.frequency === "weekly",
              mondayAtNine: sameInstant(create?.arguments.startsAt, nextMondayNine),
              pointKind: create?.arguments.kind === "point",
            },
            quality: strictWriteAssertions(observed, "calendar_create"),
          };
        },
      },
      {
        id: "pause_weekly",
        phrase: "暂停周会提醒",
        respond(call) {
          if (call.name === "calendar_find") {
            return {
              ok: true,
              total: 1,
              requiresDisambiguation: false,
              candidates: [eventFixture({
                id: "weekly-standup",
                title: "周会",
                startsAt: nextMondayNine,
                recurrence: { frequency: "weekly", weekday: 1 },
              })],
            };
          }
          if (call.name === "calendar_pause_series") return writeResult("已暂停周会系列。");
          return unexpected(call);
        },
        assert(observed) {
          const pause = firstCall(observed, "calendar_pause_series");
          return {
            functional: {
              findThenPause: observed.toolCalls.map((call) => call.name).join(",") === "calendar_find,calendar_pause_series",
              targetsSeries: pause?.arguments.eventId === "weekly-standup",
            },
            quality: strictWriteAssertions(observed, "calendar_pause_series"),
          };
        },
      },
      {
        id: "resume_weekly",
        phrase: "恢复周会提醒",
        respond(call) {
          if (call.name === "calendar_find") {
            return {
              ok: true,
              total: 1,
              requiresDisambiguation: false,
              candidates: [eventFixture({
                id: "weekly-standup",
                title: "周会",
                startsAt: nextMondayNine,
                recurrence: { frequency: "weekly", weekday: 1 },
              })],
            };
          }
          if (call.name === "calendar_resume_series") return writeResult("已恢复周会系列。");
          return unexpected(call);
        },
        assert(observed) {
          const resume = firstCall(observed, "calendar_resume_series");
          return {
            functional: {
              findThenResume: observed.toolCalls.map((call) => call.name).join(",") === "calendar_find,calendar_resume_series",
              targetsSeries: resume?.arguments.eventId === "weekly-standup",
            },
            quality: strictWriteAssertions(observed, "calendar_resume_series"),
          };
        },
      },
      {
        id: "skip_weekly_occurrence",
        phrase: "跳过下周一的周会",
        respond(call) {
          if (call.name === "calendar_find") {
            return {
              ok: true,
              total: 1,
              requiresDisambiguation: false,
              candidates: [eventFixture({
                id: "weekly-standup",
                title: "周会",
                startsAt: nextMondayNine,
                recurrence: { frequency: "weekly", weekday: 1 },
              })],
            };
          }
          if (call.name === "calendar_skip_occurrence") {
            return writeResult("已跳过下周一的周会。", { undoOperationId: "undo-weekly-skip" });
          }
          return unexpected(call);
        },
        assert(observed) {
          const skip = firstCall(observed, "calendar_skip_occurrence");
          return {
            functional: {
              findThenSkip: observed.toolCalls.map((call) => call.name).join(",") === "calendar_find,calendar_skip_occurrence",
              targetsSeries: skip?.arguments.eventId === "weekly-standup",
              targetsOccurrence: sameInstant(skip?.arguments.originalStartAt, nextMondayNine),
            },
            quality: strictWriteAssertions(observed, "calendar_skip_occurrence"),
          };
        },
      },
      {
        id: "request_terminate_weekly",
        phrase: "终止周会提醒",
        respond(call) {
          if (call.name === "calendar_find") {
            return {
              ok: true,
              total: 1,
              requiresDisambiguation: false,
              candidates: [eventFixture({
                id: "weekly-standup",
                title: "周会",
                startsAt: nextMondayNine,
                recurrence: { frequency: "weekly", weekday: 1 },
              })],
            };
          }
          if (call.name === "calendar_terminate_series") {
            if (call.arguments.confirmed === true) return writeResult("已终止周会系列。");
            return {
              ok: false,
              message: "终止后不会再生成后续周会，确认终止吗？",
              speech: "终止后不会再生成后续周会，确认终止吗？",
              requiresConfirmation: true,
            };
          }
          return unexpected(call);
        },
        assert(observed) {
          const terminate = firstCall(observed, "calendar_terminate_series");
          return {
            functional: {
              findThenTerminate: observed.toolCalls.map((call) => call.name).join(",") === "calendar_find,calendar_terminate_series",
              targetsSeries: terminate?.arguments.eventId === "weekly-standup",
              firstAttemptUnconfirmed: terminate?.arguments.confirmed !== true,
              asksForConfirmation: /(确认|是否).*(终止)|终止.*[吗？?]/u.test(observed.spokenText),
            },
            quality: { noInternalNarration: noInternalNarration(observed.spokenText) },
          };
        },
      },
      {
        id: "confirm_terminate_weekly",
        phrase: "确认终止",
        respond(call) {
          if (call.name !== "calendar_terminate_series") return unexpected(call);
          return writeResult("已终止周会系列。");
        },
        assert(observed) {
          const terminate = firstCall(observed, "calendar_terminate_series");
          return {
            functional: {
              onlyTerminateCalled: observed.toolCalls.length === 1 && Boolean(terminate),
              confirmsSameSeries: terminate?.arguments.eventId === "weekly-standup"
                && terminate?.arguments.confirmed === true,
            },
            quality: strictWriteAssertions(observed, "calendar_terminate_series"),
          };
        },
      },
    ],
  },
  {
    id: "time_block_modify_conflict",
    createState: () => ({ initialModify: null, token: "modify-conflict-token" }),
    turns: [
      {
        id: "create_complete_time_block",
        phrase: "明天下午三点到四点安排项目评审会",
        respond(call) {
          if (call.name !== "calendar_create") return unexpected(call);
          return writeResult(`已创建项目评审会，时间是${tomorrowFifteen}到${tomorrowSixteen}`, {
            eventId: "event-review",
            reminderId: "reminder-review",
            undoOperationId: "undo-review-create",
          });
        },
        assert(observed) {
          const create = firstCall(observed, "calendar_create");
          const hasEnd = sameInstant(create?.arguments.endsAt, tomorrowSixteen);
          const hasDuration = create?.arguments.durationMinutes === 60;
          return {
            functional: {
              onlyCreateCalled: observed.toolCalls.length === 1 && Boolean(create),
              timeBlockKind: create?.arguments.kind === "time_block",
              startsAtThree: sameInstant(create?.arguments.startsAt, tomorrowFifteen),
              exactOneEndRepresentation: hasEnd !== hasDuration,
            },
            quality: strictWriteAssertions(observed, "calendar_create"),
          };
        },
      },
      {
        id: "request_conflicting_modify",
        phrase: "把项目评审会改到明天上午十点",
        respond(call, state) {
          if (call.name === "calendar_find") {
            return {
              ok: true,
              total: 1,
              requiresDisambiguation: false,
              candidates: [eventFixture({ id: "event-review", title: "项目评审会", startsAt: tomorrowFifteen, kind: "time_block" })],
            };
          }
          if (call.name === "calendar_modify") {
            state.initialModify = structuredClone(call.arguments);
            return {
              ok: false,
              message: "时间与已有日程冲突",
              speech: "时间与已有日程冲突",
              reason: "calendar_conflict",
              requiresConfirmation: true,
              confirmationToken: state.token,
              conflictConfirmationToken: state.token,
            };
          }
          return unexpected(call);
        },
        assert(observed) {
          const modify = firstCall(observed, "calendar_modify");
          const movedAt = modify?.arguments.newStartAt || modify?.arguments.startsAt;
          return {
            functional: {
              findThenModify: observed.toolCalls.map((call) => call.name).join(",") === "calendar_find,calendar_modify",
              targetsReview: modify?.arguments.eventId === "event-review",
              movesToTen: sameInstant(movedAt, tomorrowTen),
              noTokenOnFirstAttempt: !modify?.arguments.conflictConfirmationToken,
              asksForConflictConfirmation: observed.spokenText === "时间与已有日程冲突，是否仍要创建？"
                || /(冲突|撞期).*[吗？?]/u.test(observed.spokenText),
            },
            quality: { noInternalNarration: noInternalNarration(observed.spokenText) },
          };
        },
      },
      {
        id: "confirm_conflicting_modify",
        phrase: "仍然修改",
        respond(call, state) {
          if (call.name !== "calendar_modify") return unexpected(call);
          if (call.arguments.conflictConfirmationToken !== state.token) {
            return { ok: false, message: "冲突确认令牌无效" };
          }
          return writeResult(`已修改项目评审会，时间是${tomorrowTen}`, {
            undoOperationId: "undo-review-modify",
          });
        },
        assert(observed, state) {
          const modify = firstCall(observed, "calendar_modify");
          const movedAt = modify?.arguments.newStartAt || modify?.arguments.startsAt;
          const initialMovedAt = state.initialModify?.newStartAt || state.initialModify?.startsAt;
          return {
            functional: {
              onlyModifyCalled: observed.toolCalls.length === 1 && Boolean(modify),
              reusesConflictToken: modify?.arguments.conflictConfirmationToken === state.token,
              preservesBusinessPayload: modify?.arguments.eventId === state.initialModify?.eventId
                && sameInstant(movedAt, initialMovedAt),
            },
            quality: strictWriteAssertions(observed, "calendar_modify"),
          };
        },
      },
    ],
  },
  {
    id: "notes_roundtrip",
    turns: [
      {
        id: "record_note",
        phrase: "记一下我把门禁卡放在书桌抽屉里",
        respond(call) {
          if (call.name !== "note_record") return unexpected(call);
          return writeResult("记住了：门禁卡放在书桌抽屉里。这条临时记录保留二十四小时。", {
            noteId: "note-card-location",
            expiresAt: at(tomorrow, "08:00:00"),
          });
        },
        assert(observed) {
          const note = firstCall(observed, "note_record");
          return {
            functional: {
              onlyRecordCalled: observed.toolCalls.length === 1 && Boolean(note),
              recordsCardLocation: typeof note?.arguments.content === "string"
                && note.arguments.content.includes("门禁卡")
                && note.arguments.content.includes("抽屉"),
            },
            quality: strictWriteAssertions(observed, "note_record"),
          };
        },
      },
      {
        id: "query_note",
        phrase: "我刚才把门禁卡放哪了",
        respond(call) {
          if (call.name !== "note_query") return unexpected(call);
          return {
            ok: true,
            speech: "门禁卡放在书桌抽屉里。",
            total: 1,
            notes: [{ noteId: "note-card-location", content: "门禁卡放在书桌抽屉里" }],
          };
        },
        assert(observed) {
          const query = firstCall(observed, "note_query");
          return {
            functional: {
              onlyQueryCalled: observed.toolCalls.length === 1 && Boolean(query),
              queriesCard: !query?.arguments.query || query.arguments.query.includes("门禁卡"),
              reportsLocation: observed.spokenText.includes("书桌抽屉"),
            },
            quality: { noInternalNarration: noInternalNarration(observed.spokenText) },
          };
        },
      },
    ],
  },
  {
    id: "entire_series_scope_followup",
    turns: [
      {
        id: "reject_single_occurrence_modify",
        phrase: "只把下周一的周会改到上午十点",
        respond: unexpected,
        assert(observed) {
          return {
            functional: {
              noToolCalled: observed.toolCalls.length === 0,
              explainsOnlyEntireSeries: /(只支持|只能|暂不支持).*(整个|整条|系列)|整个系列/u.test(observed.spokenText),
              doesNotClaimModified: !observed.spokenText.includes("已修改"),
            },
            quality: { noInternalNarration: noInternalNarration(observed.spokenText) },
          };
        },
      },
      {
        id: "accept_entire_series_modify",
        phrase: "那就把整个周会系列改到上午十点",
        respond(call) {
          if (call.name === "calendar_find") {
            return {
              ok: true,
              total: 1,
              requiresDisambiguation: false,
              candidates: [eventFixture({
                id: "weekly-scope",
                title: "周会",
                startsAt: nextMondayNine,
                recurrence: { frequency: "weekly", weekday: 1 },
              })],
            };
          }
          if (call.name === "calendar_modify") {
            return writeResult(`已修改整个周会系列，时间是${nextMondayTen}`, {
              undoOperationId: "undo-entire-series",
            });
          }
          return unexpected(call);
        },
        assert(observed) {
          const modify = firstCall(observed, "calendar_modify");
          const movedAt = modify?.arguments.newStartAt || modify?.arguments.startsAt;
          return {
            functional: {
              findThenModify: observed.toolCalls.map((call) => call.name).join(",") === "calendar_find,calendar_modify",
              modifiesEntireSeries: modify?.arguments.eventId === "weekly-scope"
                && modify?.arguments.scope === "entire_series",
              movesToTen: sameInstant(movedAt, nextMondayTen),
            },
            quality: strictWriteAssertions(observed, "calendar_modify"),
          };
        },
      },
    ],
  },
  {
    id: "acceptance_tc01_tc04",
    createState: () => ({ deleteToken: "delete-tc04-event-meeting" }),
    turns: [
      {
        id: "tc01_create_half_hour_meeting",
        phrase: "我今天下午7点要开半小时的会",
        respond(call) {
          if (call.name !== "calendar_create") return unexpected(call);
          return writeResult("已创建开会，今天下午七点到七点半。", {
            eventId: "tc04-event-meeting",
            reminderId: "tc04-reminder-meeting",
            undoOperationId: "tc04-undo-create",
          });
        },
        assert(observed) {
          const create = firstCall(observed, "calendar_create");
          return {
            functional: {
              onlyCreateCalled: observed.toolCalls.length === 1 && Boolean(create),
              startsAtSevenPm: sameInstant(create?.arguments.startsAt, todayNineteen),
              exactThirtyMinuteBlock: create?.arguments.kind === "time_block"
                && create?.arguments.durationMinutes === 30
                && !create?.arguments.endsAt,
            },
            quality: strictWriteAssertions(observed, "calendar_create"),
          };
        },
      },
      {
        id: "tc02_query_same_seven_pm_meeting",
        phrase: "我今天7点要干什么",
        respond(call) {
          if (call.name !== "calendar_query") return unexpected(call);
          return {
            ok: true,
            speech: "今天下午七点有一条安排：开会。",
            total: 1,
            occurrences: [{
              ...eventFixture({ id: "tc04-event-meeting", title: "开会", startsAt: todayNineteen, kind: "time_block" }),
              endsAt: todayNineteenThirty,
            }],
          };
        },
        assert(observed) {
          const query = firstCall(observed, "calendar_query");
          return {
            functional: {
              onlyQueryCalled: observed.toolCalls.length === 1 && Boolean(query),
              queriesSevenPmMinute: sameInstant(query?.arguments.rangeStart, todayNineteen)
                && sameInstant(query?.arguments.rangeEnd, todayNineteenOneMinute),
              reportsCreatedMeeting: observed.spokenText === "今天下午七点有一条安排：开会。",
            },
            quality: { noInternalNarration: noInternalNarration(observed.spokenText) },
          };
        },
      },
      {
        id: "tc03_move_seven_pm_meeting_to_seven_thirty",
        phrase: "我今天七点的会推迟到七点半",
        respond(call) {
          if (call.name === "calendar_find") {
            return {
              ok: true,
              speech: "找到一条日程。",
              total: 1,
              requiresDisambiguation: false,
              candidates: [{
                ...eventFixture({ id: "tc04-event-meeting", title: "开会", startsAt: todayNineteen, kind: "time_block" }),
                endsAt: todayNineteenThirty,
              }],
            };
          }
          if (call.name === "calendar_modify") {
            return writeResult("已修改开会，今天下午七点半到八点。", {
              undoOperationId: "tc04-undo-modify",
            });
          }
          return unexpected(call);
        },
        assert(observed) {
          const find = firstCall(observed, "calendar_find");
          const modify = firstCall(observed, "calendar_modify");
          const movedAt = modify?.arguments.newStartAt || modify?.arguments.startsAt;
          return {
            functional: {
              findThenModify: observed.toolCalls.map((call) => call.name).join(",") === "calendar_find,calendar_modify",
              findsSevenPmMeeting: sameInstant(find?.arguments.rangeStart, todayNineteen)
                && sameInstant(find?.arguments.rangeEnd, todayNineteenOneMinute),
              targetsCreatedMeeting: modify?.arguments.eventId === "tc04-event-meeting",
              movesToSevenThirtyPm: sameInstant(movedAt, todayNineteenThirty),
            },
            quality: strictWriteAssertions(observed, "calendar_modify"),
          };
        },
      },
      {
        id: "tc04_request_delete_moved_meeting",
        phrase: "我今天7点半的会不开了",
        respond(call, state) {
          if (call.name === "calendar_find") {
            return {
              ok: true,
              speech: "找到一条日程。",
              total: 1,
              requiresDisambiguation: false,
              candidates: [{
                ...eventFixture({ id: "tc04-event-meeting", title: "开会", startsAt: todayNineteenThirty, kind: "time_block" }),
                endsAt: todayTwenty,
              }],
            };
          }
          if (call.name === "calendar_delete") {
            if (call.arguments.confirmationToken === state.deleteToken) {
              return writeResult("已删除开会。", { undoOperationId: "tc04-undo-delete" });
            }
            return {
              ok: false,
              message: "是否确认删除这条日程？",
              speech: "是否确认删除这条日程？",
              requiresConfirmation: true,
              confirmationToken: state.deleteToken,
            };
          }
          return unexpected(call);
        },
        assert(observed) {
          const find = firstCall(observed, "calendar_find");
          const deletion = firstCall(observed, "calendar_delete");
          return {
            functional: {
              findThenDelete: observed.toolCalls.map((call) => call.name).join(",") === "calendar_find,calendar_delete",
              findsSevenThirtyPmMeeting: sameInstant(find?.arguments.rangeStart, todayNineteenThirty)
                && sameInstant(find?.arguments.rangeEnd, todayNineteenThirtyOneMinute),
              targetsMovedMeeting: deletion?.arguments.eventId === "tc04-event-meeting",
              firstDeleteUnconfirmed: !deletion?.arguments.confirmationToken,
              asksForConfirmation: /(确认|是否).*(删除)|删除.*[吗？?]/u.test(observed.spokenText),
            },
            quality: { noInternalNarration: noInternalNarration(observed.spokenText) },
          };
        },
      },
      {
        id: "tc04_confirm_delete_moved_meeting",
        phrase: "确认删除",
        respond(call, state) {
          if (call.name !== "calendar_delete") return unexpected(call);
          if (call.arguments.confirmationToken !== state.deleteToken) {
            return { ok: false, message: "删除确认令牌无效" };
          }
          return writeResult("已删除开会。", { undoOperationId: "tc04-undo-delete" });
        },
        assert(observed, state) {
          const deletion = firstCall(observed, "calendar_delete");
          return {
            functional: {
              onlyDeleteCalled: observed.toolCalls.length === 1 && Boolean(deletion),
              reusesEventAndToken: deletion?.arguments.eventId === "tc04-event-meeting"
                && deletion?.arguments.confirmationToken === state.deleteToken,
            },
            quality: strictWriteAssertions(observed, "calendar_delete"),
          };
        },
      },
    ],
  },
  {
    id: "acceptance_tc05_weekly_skip",
    turns: [
      {
        id: "tc05_create_weekly_report",
        phrase: "每周五下午五点提醒我提交周报",
        respond(call) {
          if (call.name !== "calendar_create") return unexpected(call);
          if (!call.arguments.title || !call.arguments.startsAt) {
            return { ok: false, speech: "创建日程需要标题和开始时间。" };
          }
          const normalizedStartsAt = alignWeeklyStartAt(
            call.arguments.startsAt,
            call.arguments.recurrence?.weekday,
          );
          if (!normalizedStartsAt) return { ok: false, speech: "无法计算每周日程的首次发生时间。" };
          return writeResult("已创建每周五下午五点提交周报。", {
            eventId: "tc05-weekly-report",
            reminderId: "tc05-weekly-report-reminder",
            undoOperationId: "tc05-undo-create",
            startsAt: normalizedStartsAt,
          });
        },
        assert(observed) {
          const create = firstCall(observed, "calendar_create");
          const result = firstResult(observed, "calendar_create");
          return {
            functional: {
              onlyCreateCalled: observed.toolCalls.length === 1 && Boolean(create),
              weeklyFriday: create?.arguments.recurrence?.frequency === "weekly"
                && create?.arguments.recurrence?.weekday === 5,
              firstFridayAtFive: sameInstant(result?.startsAt, nextFridaySeventeen),
              pointKind: create?.arguments.kind === "point",
            },
            quality: strictWriteAssertions(observed, "calendar_create"),
          };
        },
      },
      {
        id: "tc05_skip_this_week_report",
        phrase: "这周周报跳过一次",
        respond(call) {
          if (call.name === "calendar_find") {
            return {
              ok: true,
              speech: "找到一条日程。",
              total: 1,
              requiresDisambiguation: false,
              candidates: [eventFixture({
                id: "tc05-weekly-report",
                title: "提交周报",
                startsAt: nextFridaySeventeen,
                recurrence: { frequency: "weekly", weekday: 5 },
              })],
            };
          }
          if (call.name === "calendar_skip_occurrence") {
            return writeResult("已跳过这周的周报，下一次仍会提醒。", { undoOperationId: "tc05-undo-skip" });
          }
          return unexpected(call);
        },
        assert(observed) {
          const find = firstCall(observed, "calendar_find");
          const skip = firstCall(observed, "calendar_skip_occurrence");
          const findStart = Date.parse(find?.arguments.rangeStart);
          const findEnd = Date.parse(find?.arguments.rangeEnd);
          const occurrence = Date.parse(nextFridaySeventeen);
          return {
            functional: {
              findThenSkip: observed.toolCalls.map((call) => call.name).join(",") === "calendar_find,calendar_skip_occurrence",
              boundsThisWeekOccurrence: Number.isFinite(findStart)
                && Number.isFinite(findEnd)
                && findStart <= occurrence
                && occurrence < findEnd
                && findEnd - findStart <= 7 * 24 * 60 * 60 * 1000,
              targetsWeeklyReport: skip?.arguments.eventId === "tc05-weekly-report",
              targetsThisFriday: sameInstant(skip?.arguments.originalStartAt, nextFridaySeventeen),
              noSingleEventConfirmation: skip?.arguments.confirmed !== true,
            },
            quality: strictWriteAssertions(observed, "calendar_skip_occurrence"),
          };
        },
      },
    ],
  },
  {
    id: "acceptance_tc06_single_skip",
    turns: [
      {
        id: "tc06_create_pc_meeting",
        phrase: "明天下午三点和 PC 开会",
        respond(call) {
          if (call.name !== "calendar_create") return unexpected(call);
          return writeResult("已创建和 PC 开会，明天下午三点。", {
            eventId: "tc06-pc-meeting",
            reminderId: "tc06-pc-meeting-reminder",
            undoOperationId: "tc06-undo-create",
          });
        },
        assert(observed) {
          const create = firstCall(observed, "calendar_create");
          return {
            functional: {
              onlyCreateCalled: observed.toolCalls.length === 1 && Boolean(create),
              tomorrowAtThree: sameInstant(create?.arguments.startsAt, tomorrowFifteen),
              noDurationPoint: create?.arguments.kind === "point"
                && !create?.arguments.durationMinutes
                && !create?.arguments.endsAt,
              titleIncludesPc: /p\s*c/iu.test(create?.arguments.title || ""),
            },
            quality: strictWriteAssertions(observed, "calendar_create"),
          };
        },
      },
      {
        id: "tc06_request_skip_pc_meeting",
        phrase: "跳过明天下午三点和 PC 的会",
        respond(call) {
          if (call.name === "calendar_find") {
            return {
              ok: true,
              speech: "找到一条日程。",
              total: 1,
              requiresDisambiguation: false,
              candidates: [eventFixture({ id: "tc06-pc-meeting", title: "和 PC 开会", startsAt: tomorrowFifteen })],
            };
          }
          if (call.name === "calendar_skip_occurrence") {
            return writeResult("已取消和 PC 开会。", { undoOperationId: "tc06-undo-skip" });
          }
          return unexpected(call);
        },
        assert(observed) {
          const find = firstCall(observed, "calendar_find");
          const skip = firstCall(observed, "calendar_skip_occurrence");
          return {
            functional: {
              findThenSkip: observed.toolCalls.map((call) => call.name).join(",") === "calendar_find,calendar_skip_occurrence",
              findsTomorrowThreeMinute: sameInstant(find?.arguments.rangeStart, tomorrowFifteen)
                && sameInstant(find?.arguments.rangeEnd, tomorrowFifteenOneMinute),
              targetsPcMeeting: skip?.arguments.eventId === "tc06-pc-meeting",
              targetsTomorrowThree: !skip?.arguments.originalStartAt
                || sameInstant(skip?.arguments.originalStartAt, tomorrowFifteen),
              cancelsWithoutConfirmation: skip?.arguments.confirmed !== true,
            },
            quality: strictWriteAssertions(observed, "calendar_skip_occurrence"),
          };
        },
      },
    ],
  },
  {
    id: "acceptance_tc07_conflict_and_move",
    createState: () => ({ conflictToken: "tc07-create-conflict", secondCreate: null }),
    turns: [
      {
        id: "tc07_create_pz_meeting",
        phrase: "今天下午六点和pz开会",
        respond(call) {
          if (call.name !== "calendar_create") return unexpected(call);
          return writeResult("已创建和 pz 开会，今天下午六点。", {
            eventId: "tc07-pz-meeting",
            reminderId: "tc07-pz-reminder",
            undoOperationId: "tc07-undo-create-pz",
          });
        },
        assert(observed) {
          const create = firstCall(observed, "calendar_create");
          return {
            functional: {
              onlyCreateCalled: observed.toolCalls.length === 1 && Boolean(create),
              createsPzAtSix: sameInstant(create?.arguments.startsAt, todayEighteen)
                && /p\s*z/iu.test(create?.arguments.title || ""),
              pointKind: create?.arguments.kind === "point",
            },
            quality: strictWriteAssertions(observed, "calendar_create"),
          };
        },
      },
      {
        id: "tc07_request_conflicting_review",
        phrase: "今天下午六点项目评审",
        respond(call, state) {
          if (call.name !== "calendar_create") return unexpected(call);
          state.secondCreate = structuredClone(call.arguments);
          return {
            ok: false,
            message: "时间与已有日程冲突，是否仍要创建？",
            speech: "时间与已有日程冲突，是否仍要创建？",
            reason: "calendar_conflict",
            requiresConfirmation: true,
            confirmationToken: state.conflictToken,
            conflictConfirmationToken: state.conflictToken,
            conflicts: [eventFixture({ id: "tc07-pz-meeting", title: "和 pz 开会", startsAt: todayEighteen })],
          };
        },
        assert(observed) {
          const create = firstCall(observed, "calendar_create");
          return {
            functional: {
              onlyCreateCalled: observed.toolCalls.length === 1 && Boolean(create),
              createsReviewAtSix: sameInstant(create?.arguments.startsAt, todayEighteen)
                && create?.arguments.title?.includes("项目评审"),
              noTokenOnFirstAttempt: !create?.arguments.conflictConfirmationToken,
              asksForConflictConfirmation: /(冲突|撞期).*[吗？?]/u.test(observed.spokenText),
            },
            quality: { noInternalNarration: noInternalNarration(observed.spokenText) },
          };
        },
      },
      {
        id: "tc07_confirm_conflicting_review",
        phrase: "还是创建",
        respond(call, state) {
          if (call.name !== "calendar_create") return unexpected(call);
          return writeResult("已创建项目评审，今天下午六点。", {
            eventId: "tc07-project-review",
            reminderId: "tc07-review-reminder",
            undoOperationId: "tc07-undo-create-review",
          });
        },
        assert(observed, state) {
          const create = firstCall(observed, "calendar_create");
          return {
            functional: {
              onlyCreateCalled: observed.toolCalls.length === 1 && Boolean(create),
              reusesConflictToken: create?.arguments.conflictConfirmationToken === state.conflictToken,
              preservesConflictingReview: create?.arguments.title === state.secondCreate?.title
                && sameInstant(create?.arguments.startsAt, state.secondCreate?.startsAt),
            },
            quality: strictWriteAssertions(observed, "calendar_create"),
          };
        },
      },
      {
        id: "tc07_move_pz_meeting_to_eight",
        phrase: "把今天和pz的会改到下午八点",
        respond(call) {
          if (call.name === "calendar_find") {
            return {
              ok: true,
              speech: "找到一条日程。",
              total: 1,
              requiresDisambiguation: false,
              candidates: [eventFixture({ id: "tc07-pz-meeting", title: "和 pz 开会", startsAt: todayEighteen })],
            };
          }
          if (call.name === "calendar_modify") {
            return writeResult("已修改和 pz 开会，今天晚上八点。", { undoOperationId: "tc07-undo-modify" });
          }
          return unexpected(call);
        },
        assert(observed) {
          const find = firstCall(observed, "calendar_find");
          const modify = firstCall(observed, "calendar_modify");
          const movedAt = modify?.arguments.newStartAt || modify?.arguments.startsAt;
          const findStart = Date.parse(find?.arguments.rangeStart);
          const findEnd = Date.parse(find?.arguments.rangeEnd);
          const originalMeeting = Date.parse(todayEighteen);
          return {
            functional: {
              findThenModify: observed.toolCalls.map((call) => call.name).join(",") === "calendar_find,calendar_modify",
              boundsKnownMeetingToday: Number.isFinite(findStart)
                && Number.isFinite(findEnd)
                && findStart <= originalMeeting
                && originalMeeting < findEnd
                && findEnd - findStart <= 24 * 60 * 60 * 1000,
              targetsPzMeeting: modify?.arguments.eventId === "tc07-pz-meeting",
              movesToEightPm: sameInstant(movedAt, todayTwenty),
            },
            quality: strictWriteAssertions(observed, "calendar_modify"),
          };
        },
      },
    ],
  },
  {
    id: "acceptance_one_minute_reminder",
    createState: () => ({ startedAt: Date.now() }),
    turns: [
      {
        id: "create_one_minute_headphones_reminder",
        phrase: "一分钟后提醒我戴耳机",
        respond(call) {
          if (call.name !== "calendar_create") return unexpected(call);
          const startsAt = call.arguments.delayMinutes === 1
            ? new Date(Date.now() + 60_000).toISOString()
            : call.arguments.startsAt;
          return writeResult("已创建戴耳机提醒，一分钟后提醒。", {
            eventId: "acceptance-headphones",
            reminderId: "acceptance-headphones-reminder",
            undoOperationId: "acceptance-headphones-undo",
            startsAt,
          });
        },
        assert(observed, state) {
          const create = firstCall(observed, "calendar_create");
          const startEpoch = Date.parse(firstResult(observed, "calendar_create")?.startsAt);
          const delaySeconds = Number.isFinite(startEpoch) ? (startEpoch - state.startedAt) / 1000 : NaN;
          return {
            functional: {
              onlyCreateCalled: observed.toolCalls.length === 1 && Boolean(create),
              titleIncludesHeadphones: create?.arguments.title?.includes("戴耳机"),
              pointKind: create?.arguments.kind === "point",
              usesRelativeDelay: create?.arguments.delayMinutes === 1 && !create?.arguments.startsAt,
              dueAboutOneMinuteLater: delaySeconds >= 30 && delaySeconds <= 120,
              noSeparateLateReminder: !create?.arguments.remindAt
                || sameInstant(create.arguments.remindAt, create.arguments.startsAt),
            },
            quality: strictWriteAssertions(observed, "calendar_create"),
          };
        },
      },
    ],
  },
  {
    id: "query_empty_day",
    turns: [
      {
        id: "query_empty",
        phrase: "查询我后天的日程",
        respond(call) {
          if (call.name !== "calendar_query") return unexpected(call);
          return { ok: true, speech: "后天没有安排。", total: 0, occurrences: [] };
        },
        assert(observed) {
          const query = firstCall(observed, "calendar_query");
          return {
            functional: {
              onlyQueryCalled: observed.toolCalls.length === 1 && Boolean(query),
              exactNaturalDayRange: sameInstant(query?.arguments.rangeStart, dayAfterTomorrowStart)
                && sameInstant(query?.arguments.rangeEnd, dayAfterTomorrowEnd),
              saysNoSchedule: /(没有|暂无|0条|零条)/u.test(observed.spokenText),
            },
            quality: { noInternalNarration: noInternalNarration(observed.spokenText) },
          };
        },
      },
    ],
  },
];

const requestedScenarioIds = new Set((process.env.VOICE_TEST_SCENARIOS || "")
  .split(",")
  .map((value) => value.trim())
  .filter(Boolean));
const selectedScenarios = requestedScenarioIds.size === 0
  ? scenarios
  : scenarios.filter((scenario) => requestedScenarioIds.has(scenario.id));
if (selectedScenarios.length !== (requestedScenarioIds.size || scenarios.length)) {
  throw new Error("VOICE_TEST_SCENARIOS contains an unknown scenario id");
}

function otaBody() {
  return {
    version: 0,
    uuid: config.clientId,
    application: {
      name: "voicelife-pcm-multiturn-suite",
      version: "0.1.0",
      compile_time: new Date().toISOString(),
      idf_version: "macOS",
      elf_sha256: "0".repeat(64),
    },
    ota: { label: "multiturn-suite" },
    board: {
      type: "macos",
      name: "voicelife-pcm-multiturn-suite",
      ssid: "",
      rssi: 0,
      channel: 0,
      ip: "127.0.0.1",
      mac: config.deviceId,
    },
    flash_size: 0,
    minimum_free_heap_size: 0,
    mac_address: config.deviceId,
    chip_model_name: "macos",
    chip_info: { model: 0, cores: 0, revision: 0, features: 0 },
    partition_table: [],
  };
}

async function getConnection() {
  const response = await fetch(config.otaUrl, {
    method: "POST",
    headers: {
      "Activation-Version": "1",
      "Device-Id": config.deviceId,
      "Client-Id": config.clientId,
      "User-Agent": "voicelife-pcm-multiturn-suite/0.1.0",
      "Accept-Language": "zh-CN",
      "Content-Type": "application/json",
    },
    body: JSON.stringify(otaBody()),
  });
  const body = await response.json();
  const webSocketUrl = body.websocket?.url;
  const token = body.websocket?.token || body.websocket?.access_token || body.websocket_token;
  if (!response.ok || !webSocketUrl || !token) {
    throw new Error(`Linx OTA did not return a usable WebSocket configuration (HTTP ${response.status})`);
  }
  return { webSocketUrl, token };
}

function readWavPcm(wav) {
  if (wav.toString("ascii", 0, 4) !== "RIFF" || wav.toString("ascii", 8, 12) !== "WAVE") {
    throw new Error("say did not produce a RIFF/WAVE file");
  }
  let format = null;
  let pcm = null;
  for (let offset = 12; offset + 8 <= wav.length;) {
    const type = wav.toString("ascii", offset, offset + 4);
    const size = wav.readUInt32LE(offset + 4);
    const start = offset + 8;
    if (type === "fmt ") {
      format = {
        encoding: wav.readUInt16LE(start),
        channels: wav.readUInt16LE(start + 2),
        sampleRate: wav.readUInt32LE(start + 4),
        bits: wav.readUInt16LE(start + 14),
      };
    } else if (type === "data") {
      pcm = wav.subarray(start, start + size);
    }
    offset = start + size + (size % 2);
  }
  if (!format || format.encoding !== 1 || format.channels !== 1 || format.sampleRate !== 16000 || format.bits !== 16 || !pcm) {
    throw new Error(`unexpected WAV format: ${JSON.stringify(format)}`);
  }
  return pcm;
}

async function generatePcm(phrase) {
  const directory = await mkdtemp(path.join(tmpdir(), "voicelife-pcm-multiturn-"));
  const wavPath = path.join(directory, "input.wav");
  try {
    await execFileAsync("/usr/bin/say", [
      "-v", config.voice,
      "-r", "155",
      "--file-format=WAVE",
      "--data-format=LEI16@16000",
      "-o", wavPath,
      phrase,
    ]);
    const wav = await readFile(wavPath);
    return {
      pcm: readWavPcm(wav),
      wavSha256: createHash("sha256").update(wav).digest("hex"),
    };
  } finally {
    await rm(directory, { recursive: true, force: true });
  }
}

function helloMessage() {
  return JSON.stringify({
    type: "hello",
    version: 1,
    features: { mcp: true },
    transport: "websocket",
    audio_params: {
      format: "pcm",
      sample_rate: 16000,
      channels: 1,
      bit_depth: 16,
      endianness: "little",
      frame_duration: 20,
      frame_size: 320,
      sample_format: "signed_int16",
      play_buffer_duration: 1000,
    },
  });
}

function sendMcp(socket, sessionId, id, result) {
  socket.send(JSON.stringify({
    ...(sessionId ? { session_id: sessionId } : {}),
    type: "mcp",
    payload: { jsonrpc: "2.0", id, result },
  }));
}

async function streamPcm(socket, sessionId, pcm) {
  const silence = Buffer.alloc(640 * 10);
  const audio = Buffer.concat([silence, pcm, silence]);
  socket.send(JSON.stringify({ session_id: sessionId, type: "listen", state: "start", mode: "realtime" }));
  for (let offset = 0; offset < audio.length; offset += 640) {
    socket.send(audio.subarray(offset, Math.min(offset + 640, audio.length)), { binary: true });
    await new Promise((resolve) => setTimeout(resolve, 20));
  }
  socket.send(JSON.stringify({ session_id: sessionId, type: "listen", state: "stop", mode: "realtime" }));
}

function createTurnObservation() {
  return {
    sttTexts: [],
    toolCalls: [],
    toolResults: [],
    ttsTexts: [],
    ttsAudioBytes: 0,
    spokenText: "",
  };
}

class LinxSession {
  constructor({ socket, scenario, state }) {
    this.socket = socket;
    this.scenario = scenario;
    this.state = state;
    this.sessionId = null;
    this.current = null;
    this.hello = false;
    this.mcpInitialize = false;
    this.mcpToolsList = false;
    this.settled = false;
    this.ready = new Promise((resolve, reject) => {
      this.resolveReady = resolve;
      this.rejectReady = reject;
    });
    this.bind();
  }

  bind() {
    this.socket.on("open", () => this.socket.send(helloMessage()));
    this.socket.on("error", (error) => this.fail(error));
    this.socket.on("close", () => this.fail(new Error(`scenario ${this.scenario.id} WebSocket closed early`)));
    this.socket.on("message", (data, isBinary) => this.onMessage(data, isBinary));
  }

  fail(error) {
    if (!this.settled) this.rejectReady(error);
    if (this.current) this.current.finish(error);
  }

  onMessage(data, isBinary) {
    try {
      if (isBinary) {
        if (this.current) this.current.observed.ttsAudioBytes += data.length;
        return;
      }
      const message = JSON.parse(data.toString("utf8"));
      if (message.type === "hello") {
        this.hello = true;
        this.sessionId = message.session_id;
        return;
      }
      if (message.type === "stt" && this.current && typeof message.text === "string") {
        this.current.observed.sttTexts.push(message.text);
      }
      if (message.type === "tts" && this.current && message.state === "sentence_start") {
        this.current.observed.ttsTexts.push(message.text || "");
      }
      if (message.type === "tts" && this.current && message.state === "stop"
          && this.current.observed.ttsAudioBytes > 0) {
        this.current.finish();
      }
      if (message.type === "mcp") this.onMcp(message.payload || {});
    } catch (error) {
      this.fail(error);
    }
  }

  onMcp(payload) {
    if (payload.method === "initialize") {
      this.mcpInitialize = true;
      sendMcp(this.socket, this.sessionId, payload.id, {
        protocolVersion: "2024-11-05",
        capabilities: { tools: {} },
        serverInfo: { name: "VoiceLife PCB multi-turn suite", version: "0.1.0" },
      });
      return;
    }
    if (payload.method === "tools/list") {
      this.mcpToolsList = true;
      sendMcp(this.socket, this.sessionId, payload.id, { tools, nextCursor: null });
      if (!this.settled) {
        this.settled = true;
        this.resolveReady();
      }
      return;
    }
    if (payload.method !== "tools/call" || !this.current) return;
    const call = {
      name: payload.params?.name,
      arguments: payload.params?.arguments || {},
    };
    this.current.observed.toolCalls.push(call);
    const result = this.current.turn.respond(call, this.state);
    this.current.observed.toolResults.push({ name: call.name, result });
    sendMcp(this.socket, this.sessionId, payload.id, {
      content: [{ type: "text", text: JSON.stringify(result) }],
      isError: false,
    });
  }

  async runTurn(turn, audio) {
    if (this.current) throw new Error(`scenario ${this.scenario.id} already has an active turn`);
    const observed = createTurnObservation();
    const completion = new Promise((resolve, reject) => {
      const timeout = setTimeout(() => finish(new Error(`turn ${this.scenario.id}/${turn.id} timed out`)), config.timeoutMs);
      const finish = (error) => {
        if (!this.current || this.current.turn !== turn) return;
        clearTimeout(timeout);
        observed.spokenText = observed.ttsTexts.join("");
        this.current = null;
        if (error) reject(error);
        else resolve(observed);
      };
      this.current = { turn, observed, finish };
    });
    void streamPcm(this.socket, this.sessionId, audio.pcm).catch((error) => {
      if (this.current?.turn === turn) this.current.finish(error);
    });
    return completion;
  }
}

function evaluateAssertions(assertions) {
  const functional = assertions.functional || {};
  const quality = assertions.quality || {};
  return {
    functional,
    quality,
    functionalPass: Object.values(functional).every(Boolean),
    qualityPass: Object.values(quality).every(Boolean),
  };
}

async function runScenario(scenario) {
  const state = scenario.createState?.() || {};
  const [connection, audioInputs] = await Promise.all([
    getConnection(),
    Promise.all(scenario.turns.map((turn) => generatePcm(turn.phrase))),
  ]);
  const socket = new WebSocket(connection.webSocketUrl, {
    headers: {
      Authorization: `Bearer ${connection.token}`,
      "Device-Id": config.deviceId,
      "Client-Id": config.clientId,
      "Protocol-Version": "1",
      "X-Agent-ID": config.agentId,
    },
  });
  const session = new LinxSession({ socket, scenario, state });
  try {
    await session.ready;
    const turnResults = [];
    for (let index = 0; index < scenario.turns.length; index += 1) {
      const turn = scenario.turns[index];
      console.error(`  [${index + 1}/${scenario.turns.length}] ${turn.id}`);
      const observed = await session.runTurn(turn, audioInputs[index]);
      const assertions = evaluateAssertions(turn.assert(observed, state));
      turnResults.push({
        id: turn.id,
        phrase: turn.phrase,
        result: assertions.functionalPass ? (assertions.qualityPass ? "pass" : "pass_with_quality_gaps") : "fail",
        input: {
          format: "pcm_s16le",
          sampleRate: 16000,
          channels: 1,
          bytes: audioInputs[index].pcm.length,
          wavSha256: audioInputs[index].wavSha256,
        },
        observed,
        functionalAssertions: assertions.functional,
        qualityAssertions: assertions.quality,
      });
      if (index + 1 < scenario.turns.length) {
        await new Promise((resolve) => setTimeout(resolve, config.turnGapMs));
      }
    }
    const functionalPass = turnResults.every((turn) => turn.result !== "fail");
    const qualityPass = turnResults.every((turn) => turn.result === "pass");
    return {
      id: scenario.id,
      result: functionalPass ? (qualityPass ? "pass" : "pass_with_quality_gaps") : "fail",
      hello: session.hello,
      mcpInitialize: session.mcpInitialize,
      mcpToolsList: session.mcpToolsList,
      turnCount: turnResults.length,
      functionalTurnsPassed: turnResults.filter((turn) => turn.result !== "fail").length,
      strictTurnsPassed: turnResults.filter((turn) => turn.result === "pass").length,
      turns: turnResults,
    };
  } finally {
    socket.close();
  }
}

async function run() {
  const results = [];
  for (let index = 0; index < selectedScenarios.length; index += 1) {
    const scenario = selectedScenarios[index];
    console.error(`[${index + 1}/${selectedScenarios.length}] ${scenario.id}`);
    try {
      results.push(await runScenario(scenario));
    } catch (error) {
      results.push({ id: scenario.id, result: "fail", error: error.message });
    }
  }
  const allTurns = results.flatMap((scenario) => scenario.turns || []);
  const functionalScenariosPassed = results.filter((scenario) => scenario.result !== "fail").length;
  const strictScenariosPassed = results.filter((scenario) => scenario.result === "pass").length;
  const functionalTurnsPassed = allTurns.filter((turn) => turn.result !== "fail").length;
  const strictTurnsPassed = allTurns.filter((turn) => turn.result === "pass").length;
  const functionalResult = functionalScenariosPassed === results.length ? "pass" : "fail";
  const qualityResult = strictScenariosPassed === results.length ? "pass" : "fail";
  const manifest = {
    result: functionalResult === "pass" && qualityResult === "pass" ? "pass"
      : (functionalResult === "pass" ? "pass_with_quality_gaps" : "fail"),
    functionalResult,
    qualityResult,
    capturedAt: new Date().toISOString(),
    agentId: config.agentId,
    voice: config.voice,
    scenarioCount: results.length,
    turnCount: allTurns.length,
    functionalScenariosPassed,
    strictScenariosPassed,
    functionalTurnsPassed,
    strictTurnsPassed,
    scenarios: results,
    credentials: {
      otaTokenReceived: results.some((scenario) => Array.isArray(scenario.turns)),
      tokenPersisted: false,
      tokenLogged: false,
    },
    limitations: [
      "Every utterance is synthesized to PCM and sent through the real Linx ASR/Agent/TTS path.",
      "MCP tool responses are deterministic fixtures; this suite never mutates the physical PCB calendar.",
      "Physical persistence and automatic playback are verified by the separate hardware smoke suite.",
    ],
  };
  const manifestPath = process.env.VOICE_TEST_MANIFEST?.trim();
  if (manifestPath) await writeFile(manifestPath, `${JSON.stringify(manifest, null, 2)}\n`, { mode: 0o600 });
  console.log(JSON.stringify(manifest, null, 2));
  if (manifest.functionalResult !== "pass"
      || (process.env.VOICE_TEST_STRICT === "true" && manifest.qualityResult !== "pass")) {
    process.exitCode = 1;
  }
}

await run();
