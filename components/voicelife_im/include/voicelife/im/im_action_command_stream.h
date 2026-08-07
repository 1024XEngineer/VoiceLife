#pragma once

#include <string>

#include "voicelife/contracts/im/reminder_action_command.h"

namespace voicelife::im {

/// 动作流单次读取的结果分类。
enum class StreamReadStatus {
    /// 读取到一条动作命令。
    kCommand,
    /// 服务端正常关闭连接，无更多命令。
    kEndOfStream,
    /// 网络/TLS 中断，应重连重放未确认命令。
    kNetworkError,
    /// 协议错误（坏帧、单帧超限），连接已关闭，按可重连处理。
    kProtocolError,
};

/// 一次读取的返回：携带状态，命令仅在 kCommand 时有意义。
struct StreamRead {
    StreamReadStatus status = StreamReadStatus::kEndOfStream;
    contracts::im::ReminderActionCommand command;
};

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
     * @return 连接是否成功建立。失败（网络、TLS、鉴权等）由调用方按
     *         可重连处理，不得与「正常空流结束」混淆。
     */
    virtual bool Open(const std::string& last_event_id) = 0;
    /**
     * @brief 拉取下一条动作命令。
     * @return 携带读取状态的 StreamRead；网络中断与协议错误与正常
     *         流结束区分，调用方据此决定是否重连。
     */
    virtual StreamRead Next() = 0;
    /** @brief 关闭当前连接，释放流侧资源。 */
    virtual void Close() = 0;
};

}  // namespace voicelife::im
