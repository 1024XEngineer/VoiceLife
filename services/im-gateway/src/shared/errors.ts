/** IM Gateway 对外暴露的稳定错误码。 */
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

/** 携带稳定错误码和重试语义的网关错误。 */
export class ImGatewayError extends Error {
    /**
     * 创建带稳定错误码的网关错误。
     * @param code 稳定错误码。
     * @param message 供日志和诊断使用的错误信息。
     * @param retryable 调用方是否可以安全重试。
     */
    public constructor(
        public readonly code: ImGatewayErrorCode,
        message: string,
        public readonly retryable = false,
    ) {
        super(message);
        this.name = 'ImGatewayError';
    }
}
