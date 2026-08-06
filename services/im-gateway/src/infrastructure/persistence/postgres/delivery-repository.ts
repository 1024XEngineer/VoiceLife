import type {
    BindingId,
    ChannelAccountId,
    DeliveryId,
    DeviceId,
    EventId,
    ReminderTriggerId,
} from '../../../contracts/ids.js';
import type { Delivery, DeliveryAttempt, DeliveryReceipt } from '../../../domain/models.js';
import type { DeliveryRepository } from '../../../ports/repositories.js';
import type { IsoDateTime } from '../../../shared/types.js';
import { mapDelivery, mapDeliveryAttempt, mapDeliveryReceipt } from './mappers.js';
import { queryOne, toJson, upsert, type SqlExecutor } from './sql.js';

const DELIVERY_COLUMNS = [
    'id',
    'business_event_id',
    'correlation_id',
    'binding_id',
    'channel_account_id',
    'kind',
    'semantic_payload',
    'presentation_type',
    'status',
    'external_message_id',
    'expires_at',
    'last_error_code',
    'created_at',
    'updated_at',
] as const;

const ATTEMPT_COLUMNS = [
    'id',
    'delivery_id',
    'attempt_no',
    'request_id',
    'rendered_payload',
    'status',
    'platform_message_id',
    'error_code',
    'started_at',
    'completed_at',
] as const;

const RECEIPT_COLUMNS = [
    'id',
    'delivery_id',
    'attempt_id',
    'stage',
    'dedupe_key',
    'external_event_id',
    'detail',
    'occurred_at',
    'received_at',
] as const;

/** 投递聚合（含尝试与回执）的 PostgreSQL 实现。 */
export class PostgresDeliveryRepository implements DeliveryRepository {
    /** @param executor 事务客户端或连接池。 */
    public constructor(private readonly executor: SqlExecutor) {}

    /** {@inheritDoc DeliveryRepository.findById} */
    public async findById(id: DeliveryId): Promise<Delivery | undefined> {
        const row = await queryOne(this.executor, 'SELECT * FROM im_deliveries WHERE id = $1', [id]);
        return row === undefined ? undefined : mapDelivery(row);
    }

    /** {@inheritDoc DeliveryRepository.findByExternalMessage} */
    public async findByExternalMessage(
        channelAccountId: ChannelAccountId,
        externalMessageId: string,
    ): Promise<Delivery | undefined> {
        const row = await queryOne(
            this.executor,
            'SELECT * FROM im_deliveries WHERE channel_account_id = $1 AND external_message_id = $2 LIMIT 1',
            [channelAccountId, externalMessageId],
        );
        if (row !== undefined) return mapDelivery(row);
        const viaAttempt = await queryOne(
            this.executor,
            `SELECT d.* FROM im_delivery_attempts a JOIN im_deliveries d ON d.id = a.delivery_id
             WHERE a.platform_message_id = $1 AND d.channel_account_id = $2 LIMIT 1`,
            [externalMessageId, channelAccountId],
        );
        return viaAttempt === undefined ? undefined : mapDelivery(viaAttempt);
    }

    /** {@inheritDoc DeliveryRepository.findActiveActionWindow} */
    public async findActiveActionWindow(
        deviceId: DeviceId,
        reminderTriggerId: ReminderTriggerId,
        now: IsoDateTime,
    ): Promise<Delivery | undefined> {
        const row = await queryOne(
            this.executor,
            `SELECT * FROM im_deliveries
             WHERE expires_at > $1
               AND semantic_payload ->> 'reminderType' = 'strong'
               AND semantic_payload ->> 'reminderTriggerId' = $2
               AND semantic_payload -> 'recipient' ->> 'deviceId' = $3
             ORDER BY created_at ASC, id ASC LIMIT 1`,
            [now, reminderTriggerId, deviceId],
        );
        return row === undefined ? undefined : mapDelivery(row);
    }

    /** {@inheritDoc DeliveryRepository.findByBusinessKey} */
    public async findByBusinessKey(
        businessEventId: EventId,
        bindingId: BindingId,
        kind: Delivery['kind'],
    ): Promise<Delivery | undefined> {
        const row = await queryOne(
            this.executor,
            'SELECT * FROM im_deliveries WHERE business_event_id = $1 AND binding_id = $2 AND kind = $3 LIMIT 1',
            [businessEventId, bindingId, kind],
        );
        return row === undefined ? undefined : mapDelivery(row);
    }

    /** {@inheritDoc DeliveryRepository.save} */
    public async save(delivery: Delivery): Promise<void> {
        await upsert(
            this.executor,
            'im_deliveries',
            DELIVERY_COLUMNS,
            [
                delivery.id,
                delivery.businessEventId,
                delivery.correlationId,
                delivery.bindingId,
                delivery.channelAccountId,
                delivery.kind,
                toJson(delivery.semanticPayload),
                delivery.presentationType,
                delivery.status,
                delivery.externalMessageId ?? null,
                delivery.expiresAt ?? null,
                delivery.lastErrorCode ?? null,
                delivery.createdAt,
                delivery.updatedAt,
            ],
            ['id'],
        );
    }

    /** {@inheritDoc DeliveryRepository.findAttempt} */
    public async findAttempt(deliveryId: DeliveryId, attemptNo: number): Promise<DeliveryAttempt | undefined> {
        const row = await queryOne(
            this.executor,
            'SELECT * FROM im_delivery_attempts WHERE delivery_id = $1 AND attempt_no = $2 LIMIT 1',
            [deliveryId, attemptNo],
        );
        return row === undefined ? undefined : mapDeliveryAttempt(row);
    }

    /** {@inheritDoc DeliveryRepository.nextAttemptNo} */
    public async nextAttemptNo(deliveryId: DeliveryId): Promise<number> {
        const row = await queryOne(
            this.executor,
            'SELECT COALESCE(MAX(attempt_no), 0) + 1 AS next_no FROM im_delivery_attempts WHERE delivery_id = $1',
            [deliveryId],
        );
        return (row?.next_no as number) ?? 1;
    }

    /** {@inheritDoc DeliveryRepository.listAttempts} */
    public async listAttempts(deliveryId: DeliveryId): Promise<readonly DeliveryAttempt[]> {
        const { rows } = await this.executor.query(
            'SELECT * FROM im_delivery_attempts WHERE delivery_id = $1 ORDER BY attempt_no ASC',
            [deliveryId],
        );
        return rows.map(mapDeliveryAttempt);
    }

    /** {@inheritDoc DeliveryRepository.saveAttempt} */
    public async saveAttempt(attempt: DeliveryAttempt): Promise<void> {
        await upsert(
            this.executor,
            'im_delivery_attempts',
            ATTEMPT_COLUMNS,
            [
                attempt.id,
                attempt.deliveryId,
                attempt.attemptNo,
                attempt.requestId,
                toJson(attempt.renderedPayload),
                attempt.status,
                attempt.platformMessageId ?? null,
                attempt.errorCode ?? null,
                attempt.startedAt,
                attempt.completedAt ?? null,
            ],
            ['id'],
        );
    }

    /** {@inheritDoc DeliveryRepository.findReceiptByDedupeKey} */
    public async findReceiptByDedupeKey(dedupeKey: string): Promise<DeliveryReceipt | undefined> {
        const row = await queryOne(this.executor, 'SELECT * FROM im_delivery_receipts WHERE dedupe_key = $1 LIMIT 1', [
            dedupeKey,
        ]);
        return row === undefined ? undefined : mapDeliveryReceipt(row);
    }

    /** {@inheritDoc DeliveryRepository.listReceipts} */
    public async listReceipts(deliveryId: DeliveryId): Promise<readonly DeliveryReceipt[]> {
        const { rows } = await this.executor.query(
            'SELECT * FROM im_delivery_receipts WHERE delivery_id = $1 ORDER BY occurred_at ASC, id ASC',
            [deliveryId],
        );
        return rows.map(mapDeliveryReceipt);
    }

    /** {@inheritDoc DeliveryRepository.saveReceipt} */
    public async saveReceipt(receipt: DeliveryReceipt): Promise<void> {
        await upsert(
            this.executor,
            'im_delivery_receipts',
            RECEIPT_COLUMNS,
            [
                receipt.id,
                receipt.deliveryId,
                receipt.attemptId ?? null,
                receipt.stage,
                receipt.dedupeKey,
                receipt.externalEventId ?? null,
                toJson(receipt.detail),
                receipt.occurredAt,
                receipt.receivedAt,
            ],
            ['id'],
        );
    }
}
