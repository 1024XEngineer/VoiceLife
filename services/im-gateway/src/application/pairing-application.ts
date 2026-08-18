import type { PairingSessionId } from '../contracts/ids.js';
import { MAX_PAIRING_SESSION_MINUTES, MIN_PAIRING_SESSION_MINUTES } from '../contracts/device-gateway.js';
import type { ImBinding, PairingSession } from '../domain/models.js';
import type { Clock, ExternalIdentityProtector, IdGenerator, PairingCodePort } from '../ports/external.js';
import type { ImUnitOfWork } from '../ports/repositories.js';
import { ImGatewayError } from '../shared/errors.js';
import type {
    ConfirmPairingCommand,
    CreatedPairingSession,
    CreatePairingSessionCommand,
    PairingApplication,
} from './api.js';

const DEFAULT_PAIRING_WINDOW_MINUTES = 10;
const MAX_PAIRING_CODE_ISSUE_ATTEMPTS = 8;

/** 配对会话签发、确认、取消与过期处理的默认实现。 */
export class DefaultPairingApplication implements PairingApplication {
    /**
     * 创建配对应用服务。
     * @param unitOfWork 事务工作单元。
     * @param ids 标识生成器。
     * @param clock 业务时钟。
     * @param pairingCodes 配对码端口。
     * @param identityProtector 外部身份保护端口。
     */
    public constructor(
        private readonly unitOfWork: ImUnitOfWork,
        private readonly ids: IdGenerator,
        private readonly clock: Clock,
        private readonly pairingCodes: PairingCodePort,
        private readonly identityProtector: ExternalIdentityProtector,
    ) {}

    /** {@inheritDoc PairingApplication.create} */
    public async create(command: CreatePairingSessionCommand): Promise<CreatedPairingSession> {
        if (
            command.expiresInMinutes !== undefined &&
            (!Number.isInteger(command.expiresInMinutes) ||
                command.expiresInMinutes < MIN_PAIRING_SESSION_MINUTES ||
                command.expiresInMinutes > MAX_PAIRING_SESSION_MINUTES)
        ) {
            throw new ImGatewayError(
                'invalid_contract',
                `Pairing expiry must be an integer from ${MIN_PAIRING_SESSION_MINUTES} to ${MAX_PAIRING_SESSION_MINUTES} minutes`,
            );
        }
        for (let attempt = 0; attempt < MAX_PAIRING_CODE_ISSUE_ATTEMPTS; attempt++) {
            const code = await this.pairingCodes.issue();
            const now = this.clock.now();
            const session: PairingSession = {
                id: this.ids.nextPairingSessionId(),
                displayCodeHash: code.hash,
                ...(command.userId === undefined ? {} : { userId: command.userId }),
                deviceId: command.deviceId,
                ...(command.allowedPlatforms === undefined ? {} : { allowedPlatforms: command.allowedPlatforms }),
                status: 'pending',
                expiresAt: this.clock.addMinutes(now, command.expiresInMinutes ?? DEFAULT_PAIRING_WINDOW_MINUTES),
                createdAt: now,
            };
            const created = await this.unitOfWork.transaction(async (tx) => {
                const device = await tx.devices.lockById(command.deviceId);
                if (device === undefined || device.status !== 'active') {
                    throw new ImGatewayError('unauthorized', 'Device authorization is invalid');
                }
                if (command.userId !== undefined && command.userId !== device.userId) {
                    throw new ImGatewayError('invalid_transition', 'Pairing user does not own the device');
                }
                await tx.pairingSessions.cancelPendingByDevice(command.deviceId);
                return tx.pairingSessions.createPendingIfAbsent(session);
            });
            if (created) {
                return { session, displayCode: code.displayCode };
            }
        }
        throw new ImGatewayError('resource_exhausted', 'Could not issue a unique pairing code', true);
    }

    /** {@inheritDoc PairingApplication.find} */
    public async find(pairingSessionId: PairingSessionId): Promise<PairingSession | undefined> {
        await this.expireDue();
        return this.unitOfWork.transaction((tx) => tx.pairingSessions.findById(pairingSessionId));
    }

    /** {@inheritDoc PairingApplication.confirm} */
    public async confirm(command: ConfirmPairingCommand): Promise<ImBinding> {
        const codeHash = await this.pairingCodes.hash(command.displayCode);
        const protectedIdentity = await this.identityProtector.protect(command.externalUserId);
        return this.unitOfWork.transaction(async (tx) => {
            // 普通预查询只用于取得锁顺序所需的设备与会话标识，不作为安全判断。
            const candidate = await tx.pairingSessions.findPendingByDisplayCodeHash(codeHash);
            if (candidate === undefined) {
                throw new ImGatewayError('pairing_code_invalid', 'Pairing session is invalid or expired');
            }
            const device = await tx.devices.lockById(candidate.deviceId);
            const session = await tx.pairingSessions.lockPendingByIdAndDisplayCodeHash(candidate.id, codeHash);
            const now = this.clock.now();
            if (session === undefined || session.deviceId !== candidate.deviceId || session.expiresAt <= now) {
                throw new ImGatewayError('pairing_code_invalid', 'Pairing session is invalid or expired');
            }
            await tx.bindings.acquireReplacementLock();
            const account = await tx.channelAccounts.findById(command.channelAccountId);
            if (account === undefined || account.status !== 'active') {
                throw new ImGatewayError('binding_not_found', 'Channel account was not found');
            }
            if (session.allowedPlatforms !== undefined && !session.allowedPlatforms.includes(account.platform)) {
                throw new ImGatewayError('capability_not_supported', 'Platform is not allowed by the pairing session');
            }

            if (session.userId !== undefined && command.userId !== undefined && session.userId !== command.userId) {
                throw new ImGatewayError(
                    'invalid_transition',
                    'Pairing confirmation user does not match the pairing session',
                );
            }
            const userId = session.userId ?? command.userId;
            if (userId === undefined) {
                throw new ImGatewayError('invalid_contract', 'Pairing confirmation requires an internal userId');
            }

            if (device === undefined || device.status !== 'active' || device.userId !== userId) {
                throw new ImGatewayError('invalid_transition', 'Pairing device is inactive or owned by another user');
            }

            let identity = await tx.identities.findByChannelAndHash(account.id, protectedIdentity.hash);
            if (identity === undefined) {
                identity = await tx.identities.createIfAbsent({
                    id: this.ids.nextExternalIdentityId(),
                    channelAccountId: account.id,
                    externalUserIdCiphertext: protectedIdentity.ciphertext,
                    externalUserIdHash: protectedIdentity.hash,
                    ...(command.displayName === undefined ? {} : { displayName: command.displayName }),
                    status: 'active',
                    createdAt: now,
                    updatedAt: now,
                });
            }
            if (identity.status !== 'active') {
                throw new ImGatewayError('invalid_transition', 'External identity is not active');
            }

            const binding = await tx.bindings.replaceActiveBinding({
                id: this.ids.nextBindingId(),
                userId,
                deviceId: session.deviceId,
                externalIdentityId: identity.id,
                priority: 100,
                status: 'active',
                boundAt: now,
            });
            await tx.pairingSessions.save({
                ...session,
                status: 'confirmed',
                confirmedAt: now,
            });
            return binding;
        });
    }

    /** {@inheritDoc PairingApplication.cancel} */
    public cancel(pairingSessionId: PairingSessionId): Promise<void> {
        return this.unitOfWork.transaction(async (tx) => {
            await tx.pairingSessions.transitionPending(pairingSessionId, 'cancelled');
        });
    }

    /** {@inheritDoc PairingApplication.expireDue} */
    public expireDue(): Promise<number> {
        return this.unitOfWork.transaction(async (tx) => {
            const sessions = await tx.pairingSessions.findExpiredPairingSessions(this.clock.now());
            let expired = 0;
            for (const session of sessions) {
                if (await tx.pairingSessions.transitionPending(session.id, 'expired')) expired += 1;
            }
            return expired;
        });
    }
}
