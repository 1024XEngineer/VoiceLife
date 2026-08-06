#pragma once

#include <optional>
#include <string>

#include "voicelife/contracts/im/reminder_action_command.h"

namespace voicelife::im {

/// 网关动作命令流（SSE）端口。一次实例代表一条连接的生命周期。
///
/// 断线重连由调用方创建新实例；通道会通过 Open 携带上次确认的命令
/// 游标（Last-Event-ID），网关据此重放未确认命令。
class ImActionCommandStream {
   public:
    /** @brief 允许通过接口指针释放动作流。 */
    virtual ~ImActionCommandStream() = default;
    /**
     * @brief 打开（或重连）动作命令流连接。
     * @param last_event_id 上次确认的 commandId 游标，空串表示无历史游标。
     */
    virtual void Open(const std::string& last_event_id) = 0;
    /**
     * @brief 拉取下一条动作命令。
     * @return 命令；连接结束或中断时返回 nullopt。
     */
    virtual std::optional<contracts::im::ReminderActionCommand> Next() = 0;
    /** @brief 关闭当前连接，释放流侧资源。 */
    virtual void Close() = 0;
};

}  // namespace voicelife::im
