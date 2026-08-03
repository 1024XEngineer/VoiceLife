import type {
  ActionUiApplication,
  ActionUiView,
} from "../../application/api.js";
import type {
  ActionIntent,
  ReminderActionCommand,
  ReminderActionKind,
} from "../../contracts/device-gateway.js";

export const ACTION_UI_ROUTES = {
  show: "/voicelife/reminder-actions/:token",
  execute: "/voicelife/reminder-actions/:token",
} as const;

/** H5/mini-app route. It is not a Koishi Adapter and creates no Session. */
export class ActionUiController {
  public constructor(private readonly actionUi: ActionUiApplication) {}

  public get(token: string): Promise<ActionUiView> {
    return this.actionUi.show(token);
  }

  public post(input: Pick<ActionIntent, "token" | "params"> & {
    readonly action: ReminderActionKind;
  }): Promise<ReminderActionCommand> {
    return this.actionUi.execute(input);
  }
}
