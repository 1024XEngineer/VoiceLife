import type { ImGatewayApplication } from '../application/api.js';
import {
    DefaultActionApplication,
    DefaultActionUiApplication,
    DefaultBindingApplication,
    DefaultChannelAccountApplication,
    DefaultDeliveryApplication,
    DefaultDeliveryDispatchApplication,
    DefaultInboundEventApplication,
    DefaultNotificationApplication,
    DefaultPairingApplication,
    DefaultPlatformEventApplication,
    DefaultReceiptApplication,
} from '../application/services.js';
import type { DeviceId } from '../contracts/ids.js';
import { unsafeId } from '../contracts/ids.js';
import { ActionUiController } from '../infrastructure/http/action-ui-api.js';
import { DeviceIntentController, ReminderActionStreamController } from '../infrastructure/http/device-api.js';
import {
    FixedClock,
    InMemoryActionCommandStream,
    InMemoryActionTokenPort,
    MockChannelCapabilities,
    MockChannelHealthPort,
    MockConversationResolver,
    MockDeliveryRenderer,
    MockDeviceAuthenticationPort,
    MockExternalIdentityProtector,
    MockImChannel,
    MockPairingCodePort,
    SequentialIdGenerator,
} from '../infrastructure/mock-support.js';
import { InMemoryImUnitOfWork } from '../infrastructure/persistence/in-memory.js';
import type {
    ActionCommandStreamPort,
    ActionTokenPort,
    ChannelCapabilityResolver,
    ChannelHealthPort,
    Clock,
    ConversationResolverPort,
    DeliveryRendererPort,
    DeviceAuthenticationPort,
    ExternalIdentityProtector,
    IdGenerator,
    ImChannelPort,
    PairingCodePort,
} from '../ports/external.js';
import type { ImUnitOfWork } from '../ports/repositories.js';

export interface ImGatewayDependencies {
    readonly unitOfWork: ImUnitOfWork;
    readonly actionStream: ActionCommandStreamPort;
    readonly actionTokens: ActionTokenPort;
    readonly authentication: DeviceAuthenticationPort;
    readonly channelCapabilities: ChannelCapabilityResolver;
    readonly channelHealth: ChannelHealthPort;
    readonly conversations: ConversationResolverPort;
    readonly deliveryRenderer: DeliveryRendererPort;
    readonly imChannel: ImChannelPort;
    readonly pairingCodes: PairingCodePort;
    readonly identityProtector: ExternalIdentityProtector;
    readonly clock: Clock;
    readonly ids: IdGenerator;
}

export interface ImGatewayRuntime {
    readonly application: ImGatewayApplication;
    readonly deviceApi: DeviceIntentController;
    readonly actionStreamApi: ReminderActionStreamController;
    readonly actionUiApi: ActionUiController;
}

export function createImGateway(dependencies: ImGatewayDependencies): ImGatewayRuntime {
    const channels = new DefaultChannelAccountApplication(
        dependencies.unitOfWork,
        dependencies.ids,
        dependencies.clock,
        dependencies.channelHealth,
    );
    const pairing = new DefaultPairingApplication(
        dependencies.unitOfWork,
        dependencies.ids,
        dependencies.clock,
        dependencies.pairingCodes,
        dependencies.identityProtector,
    );
    const bindings = new DefaultBindingApplication(dependencies.unitOfWork, dependencies.clock);
    const inboundEvents = new DefaultInboundEventApplication(dependencies.unitOfWork, dependencies.clock);
    const notifications = new DefaultNotificationApplication(
        dependencies.unitOfWork,
        dependencies.ids,
        dependencies.clock,
        dependencies.channelCapabilities,
    );
    const deliveries = new DefaultDeliveryApplication(dependencies.unitOfWork, dependencies.ids, dependencies.clock);
    const receipts = new DefaultReceiptApplication(dependencies.unitOfWork, dependencies.ids, dependencies.clock);
    const actions = new DefaultActionApplication(
        dependencies.unitOfWork,
        dependencies.actionStream,
        dependencies.ids,
        dependencies.clock,
    );
    const actionUi = new DefaultActionUiApplication(dependencies.actionTokens, actions, dependencies.clock);
    const deliveryDispatch = new DefaultDeliveryDispatchApplication(
        dependencies.unitOfWork,
        dependencies.ids,
        dependencies.clock,
        dependencies.channelCapabilities,
        dependencies.conversations,
        dependencies.deliveryRenderer,
        dependencies.imChannel,
        actionUi,
    );
    const platformEvents = new DefaultPlatformEventApplication(inboundEvents, pairing, receipts, actionUi);
    const application: ImGatewayApplication = {
        channels,
        pairing,
        bindings,
        inboundEvents,
        platformEvents,
        notifications,
        deliveries,
        deliveryDispatch,
        receipts,
        actions,
        actionUi,
    };

    return {
        application,
        deviceApi: new DeviceIntentController(notifications, actions, dependencies.authentication, pairing),
        actionStreamApi: new ReminderActionStreamController(
            dependencies.actionStream,
            dependencies.authentication,
            actions,
        ),
        actionUiApi: new ActionUiController(actionUi),
    };
}

export function createMockImGateway(
    deviceId: DeviceId = unsafeId<DeviceId>('device-demo'),
    clock: FixedClock = new FixedClock(),
    overrides: Partial<ImGatewayDependencies> = {},
): ImGatewayRuntime {
    return createImGateway({
        unitOfWork: new InMemoryImUnitOfWork(),
        actionStream: new InMemoryActionCommandStream(),
        actionTokens: new InMemoryActionTokenPort(),
        authentication: new MockDeviceAuthenticationPort(deviceId),
        channelCapabilities: new MockChannelCapabilities(),
        channelHealth: new MockChannelHealthPort(clock),
        conversations: new MockConversationResolver(),
        deliveryRenderer: new MockDeliveryRenderer(),
        imChannel: new MockImChannel(),
        pairingCodes: new MockPairingCodePort(),
        identityProtector: new MockExternalIdentityProtector(),
        clock,
        ids: new SequentialIdGenerator(),
        ...overrides,
    });
}
