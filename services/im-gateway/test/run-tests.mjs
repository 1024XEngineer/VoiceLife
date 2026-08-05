import { readFile } from "node:fs/promises";

import {
  ImGatewayError,
  createMockImGateway,
  parseNotificationIntent,
  parseReminderActionIntent,
  parseReminderActionResult,
  parseScheduleReceiptIntent,
  runMockNotificationScenario,
} from "../dist/index.js";
import { FixedClock } from "../dist/infrastructure/mock-support.js";

const fixtureRoot = new URL(
  "../../../contracts/im-gateway/v1/fixtures/",
  import.meta.url,
);

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

async function readFixture(name) {
  return JSON.parse(await readFile(new URL(name, fixtureRoot), "utf8"));
}

async function expectGatewayError(work, code, message) {
  try {
    await work();
  } catch (error) {
    if (error instanceof ImGatewayError && error.code === code) return;
    throw error;
  }
  throw new Error(message);
}

async function expectRejected(work, message) {
  try {
    await work();
  } catch (error) {
    return error;
  }
  throw new Error(message);
}

async function createBoundGateway(overrides = {}) {
  const clock = new FixedClock();
  const gateway = createMockImGateway("device-fixture", clock, overrides);
  const channel = await gateway.application.channels.register({
    platform: "wechat_official",
    tenantExternalId: "fixture-account",
    koishiBotId: "fixture-bot",
    credentialRef: "secret://fixture-account",
    connectionMode: "webhook",
  });
  const pairing = await gateway.deviceApi.postPairingSession({
    authorization: "Bearer fixture-device-token",
    body: { userId: "user-fixture", deviceId: "device-fixture" },
  });
  await gateway.application.pairing.confirm({
    displayCode: pairing.displayCode,
    channelAccountId: channel.id,
    externalUserId: "fixture-open-id",
  });
  return { gateway, clock };
}

async function submitFixture(gateway, body) {
  return gateway.deviceApi.postNotification({
    authorization: "Bearer fixture-device-token",
    idempotencyKey: body.businessEventId,
    body,
  });
}

async function runContractFixtureTests() {
  const scheduleReceipt = await readFixture("schedule-receipt.json");
  const strong = await readFixture("notification-strong.json");
  const replay = await readFixture("notification-strong-replay.json");
  const weak = await readFixture("notification-weak.json");
  const conflict = await readFixture("notification-conflict.json");

  assert(
    parseScheduleReceiptIntent(scheduleReceipt).scheduleId === "schedule-fixture",
    "ScheduleReceiptIntent fixture did not preserve its opaque string ID",
  );
  assert(
    parseNotificationIntent(strong).reminderType === "strong",
    "Strong notification fixture did not parse",
  );
  assert(
    parseNotificationIntent(weak).actions.length === 0,
    "Weak notification fixture did not parse without actions",
  );

  for (const name of [
    "notification-invalid-version.json",
    "notification-invalid-enum.json",
    "notification-invalid-time.json",
    "notification-missing-field.json",
  ]) {
    const invalidFixture = await readFixture(name);
    await expectGatewayError(
      () => Promise.resolve(parseNotificationIntent(invalidFixture)),
      "invalid_contract",
      `${name} was accepted by the runtime parser`,
    );
  }

  await expectGatewayError(
    () =>
      Promise.resolve(
        parseReminderActionResult({
          schemaVersion: "1",
          operationId: "operation-fixture",
          reminderTriggerId: "trigger-fixture",
          status: "succeeded",
          occurredAt: "not-a-time",
        }),
      ),
    "invalid_contract",
    "ReminderActionResult accepted an invalid ISO 8601 value",
  );
  await expectGatewayError(
    () =>
      Promise.resolve(
        parseReminderActionResult({
          schemaVersion: "1",
          operationId: "operation-fixture",
          reminderTriggerId: "trigger-fixture",
          status: "succeeded",
          occurredAt: "2026-02-31T00:00:00.000Z",
        }),
      ),
    "invalid_contract",
    "ReminderActionResult accepted an impossible calendar date",
  );
  await expectGatewayError(
    () =>
      Promise.resolve(
        parseReminderActionIntent({
          token: "fixture-token",
          action: "snooze",
          params: { minutes: 0 },
        }),
      ),
    "invalid_contract",
    "Action UI accepted a non-positive snooze duration",
  );

  const { gateway } = await createBoundGateway();
  const first = await submitFixture(gateway, strong);
  const duplicate = await submitFixture(gateway, replay);
  assert(
    first.deliveries[0]?.deliveryId === duplicate.deliveries[0]?.deliveryId,
    "Identical fixture replay did not return the original submission",
  );
  await expectGatewayError(
    () => submitFixture(gateway, conflict),
    "idempotency_conflict",
    "Conflicting fixture replay was accepted",
  );
}

async function runFailureStateTests() {
  const strong = await readFixture("notification-strong.json");
  const { gateway: deliveryGateway } = await createBoundGateway({
    imChannel: {
      send: async () => {
        throw new Error("fixture channel outage");
      },
    },
  });
  const submitted = await submitFixture(deliveryGateway, strong);
  const deliveryId = submitted.deliveries[0]?.deliveryId;
  assert(deliveryId !== undefined, "Failure fixture did not create a Delivery");
  const failed = await deliveryGateway.application.deliveryDispatch.dispatch(
    deliveryId,
  );
  const details = await deliveryGateway.application.deliveries.find(deliveryId);
  assert(
    failed.status === "retryable_failed" &&
      details?.attempts[0]?.status === "retryable_failed",
    "Thrown channel.send() did not move Delivery and Attempt to retryable_failed",
  );

  const failingStream = {
    publish: async () => {
      throw new Error("fixture stream outage");
    },
    subscribe: () => (async function* emptyStream() {})(),
    close: async () => {},
  };
  const { gateway: actionGateway } = await createBoundGateway({
    actionStream: failingStream,
  });
  const actionSubmission = await submitFixture(actionGateway, strong);
  const actionDeliveryId = actionSubmission.deliveries[0]?.deliveryId;
  assert(actionDeliveryId !== undefined, "Action fixture did not create a Delivery");
  const token = await actionGateway.application.actionUi.issue(actionDeliveryId);
  const streamError = await expectRejected(
    () => actionGateway.actionUiApi.post({ token, action: "acknowledge" }),
    "Action stream fixture did not throw",
  );
  assert(
    streamError instanceof Error && streamError.message === "fixture stream outage",
    "Action stream failure was not propagated",
  );
  const replayable = await actionGateway.application.actions.replayPending(
    "device-fixture",
    "trigger-fixture",
  );
  assert(
    replayable.length === 1,
    "A dispatched Action was not recoverable after stream.publish() failed",
  );

  const publishedCommands = [];
  const recordingStream = {
    publish: async (command) => {
      publishedCommands.push(command);
    },
    subscribe: () => (async function* emptyStream() {})(),
    close: async () => {},
  };
  const { gateway: retryGateway } = await createBoundGateway({
    actionStream: recordingStream,
  });
  const retrySubmission = await submitFixture(retryGateway, strong);
  const retryDeliveryId = retrySubmission.deliveries[0]?.deliveryId;
  assert(retryDeliveryId !== undefined, "Retry fixture did not create a Delivery");
  const retryToken = await retryGateway.application.actionUi.issue(retryDeliveryId);
  const retryCommand = await retryGateway.actionUiApi.post({
    token: retryToken,
    action: "acknowledge",
  });
  await retryGateway.deviceApi.postReminderActionResult({
    authorization: "Bearer fixture-device-token",
    deviceId: "device-fixture",
    commandId: retryCommand.commandId,
    body: {
      schemaVersion: "1",
      operationId: retryCommand.operationId,
      reminderTriggerId: retryCommand.reminderTriggerId,
      status: "retryable_failed",
      occurredAt: "2026-08-03T00:01:00.000Z",
    },
  });
  assert(
    publishedCommands.length === 2 &&
      publishedCommands[0]?.operationId === publishedCommands[1]?.operationId,
    "retryable_failed did not republish the same operation to the live stream",
  );
}

await runMockNotificationScenario();
await runContractFixtureTests();
await runFailureStateTests();
console.log("IM Gateway Issue #95 contract and review regression tests passed");
