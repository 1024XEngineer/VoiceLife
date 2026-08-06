import type { ActionUiApplication, ActionUiView } from '../../application/api.js';
import type { ReminderActionCommand } from '../../contracts/device-gateway.js';
import { parseActionToken, parseReminderActionIntent } from '../../contracts/device-gateway-parser.js';

/** 提醒动作页面的展示与执行路由。 */
export const ACTION_UI_ROUTES = {
    show: '/voicelife/reminder-actions/:token',
    execute: '/voicelife/reminder-actions/:token',
} as const;

/** H5 或小程序动作入口控制器，不依赖 Koishi Session。 */
export class ActionUiController {
    /** @param actionUi 动作页面应用服务。 */
    public constructor(private readonly actionUi: ActionUiApplication) {}

    /**
     * 校验路径令牌并返回动作页面视图。
     * @param token 未受信任的路径令牌。
     * @returns 可安全呈现的动作视图。
     */
    public get(token: unknown): Promise<ActionUiView> {
        return this.actionUi.show(parseActionToken(token));
    }

    /**
     * 校验并执行动作页面提交的操作。
     * @param input 未受信任的动作载荷。
     * @returns 下发给设备的动作命令。
     */
    public post(input: unknown): Promise<ReminderActionCommand> {
        return this.actionUi.execute(parseReminderActionIntent(input));
    }
}
