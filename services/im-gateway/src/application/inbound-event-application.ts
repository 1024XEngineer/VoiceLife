import type { NormalizedImEvent } from '../contracts/platform-events.js';
import type { Clock } from '../ports/external.js';
import type { ImUnitOfWork } from '../ports/repositories.js';
import { ImGatewayError } from '../shared/errors.js';
import type { JsonValue } from '../shared/types.js';
import type { InboundEventApplication } from './api.js';

/** 规范化入站事件幂等落库与状态推进的默认实现。 */
export class DefaultInboundEventApplication implements InboundEventApplication {
    /**
     * 创建入站事件应用服务。
     * @param unitOfWork 事务工作单元。
     * @param clock 业务时钟。
     */
    public constructor(
        private readonly unitOfWork: ImUnitOfWork,
        private readonly clock: Clock,
    ) {}

    /** {@inheritDoc InboundEventApplication.recordIfNew} */
    public recordIfNew(event: NormalizedImEvent): Promise<'accepted' | 'duplicate'> {
        return this.unitOfWork.transaction(async (tx) => {
            const duplicate = await tx.inboundEvents.findByExternalEvent(event.channelAccountId, event.externalEventId);
            if (duplicate !== undefined) {
                if (duplicate.status !== 'failed') return 'duplicate';
                await tx.inboundEvents.save({
                    ...duplicate,
                    id: event.id,
                    payload: persistedInboundPayload(event),
                    status: 'received',
                    occurredAt: event.occurredAt,
                    receivedAt: this.clock.now(),
                });
                return 'accepted';
            }
            await tx.inboundEvents.save({
                id: event.id,
                channelAccountId: event.channelAccountId,
                externalEventId: event.externalEventId,
                eventType: toInboundEventType(event.type),
                payload: persistedInboundPayload(event),
                status: 'received',
                occurredAt: event.occurredAt,
                receivedAt: this.clock.now(),
            });
            return 'accepted';
        });
    }

    /** {@inheritDoc InboundEventApplication.markProcessing} */
    public markProcessing(eventId: NormalizedImEvent['id']): Promise<void> {
        return this.updateStatus(eventId, 'processing');
    }

    /** {@inheritDoc InboundEventApplication.markProcessed} */
    public markProcessed(eventId: NormalizedImEvent['id']): Promise<void> {
        return this.updateStatus(eventId, 'processed');
    }

    /** {@inheritDoc InboundEventApplication.markFailed} */
    public markFailed(eventId: NormalizedImEvent['id']): Promise<void> {
        return this.updateStatus(eventId, 'failed');
    }

    private updateStatus(
        eventId: NormalizedImEvent['id'],
        status: 'processing' | 'processed' | 'failed',
    ): Promise<void> {
        return this.unitOfWork.transaction(async (tx) => {
            const event = await tx.inboundEvents.findById(eventId);
            if (event === undefined) {
                throw new ImGatewayError('invalid_transition', 'Inbound event was not found');
            }
            await tx.inboundEvents.save({ ...event, status });
        });
    }
}

function persistedInboundPayload(event: NormalizedImEvent): JsonValue {
    if (event.type === 'binding.requested') return { kind: 'binding_requested' };
    if (event.type === 'message.received') {
        const payload = event.payload;
        if (typeof payload !== 'object' || payload === null || Array.isArray(payload))
            return { kind: 'message_received' };
        const value = payload as Record<string, JsonValue>;
        return {
            kind: 'message_received',
            ...(typeof value.messageType === 'string' ? { messageType: value.messageType } : {}),
            ...(typeof value.event === 'string' ? { event: value.event } : {}),
        };
    }
    return event.payload as JsonValue;
}

function toInboundEventType(type: NormalizedImEvent['type']): NormalizedImEvent['type'] {
    return type;
}
