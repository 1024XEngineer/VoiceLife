import type { ActionUiApplication, ActionUiView } from '../../application/api.js';
import type { ReminderActionCommand } from '../../contracts/device-gateway.js';
import { parseActionToken, parseReminderActionIntent } from '../../contracts/device-gateway-parser.js';

export const ACTION_UI_ROUTES = {
    show: '/voicelife/reminder-actions/:token',
    execute: '/voicelife/reminder-actions/:token',
} as const;

/** H5/mini-app route. It is not a Koishi Adapter and creates no Session. */
export class ActionUiController {
    public constructor(private readonly actionUi: ActionUiApplication) {}

    public get(token: unknown): Promise<ActionUiView> {
        return this.actionUi.show(parseActionToken(token));
    }

    public post(input: unknown): Promise<ReminderActionCommand> {
        return this.actionUi.execute(parseReminderActionIntent(input));
    }
}
