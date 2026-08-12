#pragma once

#include <string>

#include "voicelife/contracts/status.h"

namespace voicelife::runtime {

/** @brief 热点配网结果：用户通过 Web 页提交的 Wi-Fi 凭据。 */
struct HotspotProvisionResult {
    std::string ssid;
    std::string password;
    /** 用户是否在超时前提交了配置（false = 超时未配，由调用方决定重试策略）。 */
    bool configured = false;
};

/**
 * @brief 启动传统热点配网：开 AP（Voicelife-XXXX）+ DNS 劫持 + Web 配置页。
 * 手机连接热点后浏览器访问 192.168.4.1，选择 Wi-Fi 并提交密码。
 * @param result 输出用户提交的凭据。
 * @param timeout_ms 配网等待上限（0 = 无限等待）。
 * @return 配网是否成功（configured=true 时返回 Ok）。
 */
Status RunHotspotProvision(HotspotProvisionResult& result, uint32_t timeout_ms = 300000);

}  // namespace voicelife::runtime
