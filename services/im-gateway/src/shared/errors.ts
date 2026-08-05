export type ImGatewayErrorCode =
    | 'invalid_contract'
    | 'idempotency_conflict'
    | 'binding_not_found'
    | 'delivery_not_found'
    | 'action_not_found'
    | 'action_expired'
    | 'duplicate_event'
    | 'invalid_transition'
    | 'capability_not_supported'
    | 'not_implemented';

export class ImGatewayError extends Error {
    public constructor(
        public readonly code: ImGatewayErrorCode,
        message: string,
        public readonly retryable = false,
    ) {
        super(message);
        this.name = 'ImGatewayError';
    }
}
