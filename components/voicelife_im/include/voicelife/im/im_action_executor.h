#pragma once

#include "voicelife/contracts/im/reminder_action_command.h"
#include "voicelife/contracts/im/reminder_action_result.h"

namespace voicelife::im {

/// 本地动作执行端口：设备侧 TimingTask Application Port 的实现契约。
///
/// 实现必须不修改命令输入；返回的结果直接作为回传载荷。
class ImActionExecutor {
   public:
    /** @brief 允许通过接口指针释放执行器。 */
    virtual ~ImActionExecutor() = default;
    /**
     * @brief 执行一条提醒动作命令并返回执行结果。
     * @param command 网关下发的动作命令，执行器不得修改。
     * @return 执行结果，含 status 与可选 nextTriggerAt/errorCode。
     */
    virtual contracts::im::ReminderActionResult Execute(const contracts::im::ReminderActionCommand& command) = 0;
};

}  // namespace voicelife::im
