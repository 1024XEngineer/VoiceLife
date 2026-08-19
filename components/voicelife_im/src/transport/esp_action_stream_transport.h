#pragma once

#include <deque>
#include <functional>
#include <string>
#include <vector>

#include "esp_http_client.h"
#include "voicelife/contracts/im/reminder_action_command.h"
#include "voicelife/im/im_action_command_stream.h"
#include "voicelife/im/im_credentials.h"
#include "voicelife/im/im_sse.h"

namespace voicelife::im {

/// 基于 esp_http_client 流式读取的动作命令流（SSE）实现。仅固件编译。
///
/// Open 建立 GET /v1/devices/{deviceId}/reminder-actions/stream 连接，携带
/// Authorization 与 Last-Event-ID 请求头；Next 阻塞读取 SSE 帧并把 reminder.action
/// 载荷解析为动作命令，以 StreamRead 区分命令、正常结束、网络错误与协议错误；
/// 连接中断或坏帧时自动关闭连接。析构时自动关闭连接。
class EspActionStreamTransport : public ImActionCommandStream {
   public:
    /**
     * @brief 创建动作流传输。
     * @param base_url 网关基地址，例如 "https://im.example.com"。
     * @param credentials 设备凭据，用于 Authorization 与 URL 中的 deviceId。
     * @param reminder_trigger_id 本次窗口绑定的提醒触发标识。
     */
    EspActionStreamTransport(std::string base_url, ImCredentialProvider& credentials, std::string reminder_trigger_id);
    ~EspActionStreamTransport() override { CloseConnection(); }
    bool Open(const std::string& last_event_id) override;
    StreamRead Next() override;
    void Close() override;

    /**
     * @brief 设置停止检查回调。
     *
     * 网关心跳使 Next 的阻塞读取永不超时；设置后每次读取前检查，命中时
     * 关闭连接并返回网络错误，使停止延迟有界（不超过一次读取）。可为空清除检查。
     * @param check 返回 true 表示应停止读取并断开连接。
     */
    void SetStopCheck(std::function<bool()> check);

   private:
    void CloseConnection();

    std::string base_url_;
    ImCredentialProvider& credentials_;
    std::string reminder_trigger_id_;
    std::function<bool()> stop_check_;
    esp_http_client_handle_t client_ = nullptr;
    SseDecoder decoder_;
    std::deque<SseFrame> pending_;
    bool open_ = false;
};

}  // namespace voicelife::im
