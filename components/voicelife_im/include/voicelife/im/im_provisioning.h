#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include "voicelife/contracts/status.h"

namespace voicelife::im {

/// 物理串口 IM provisioning 帧的固定头长度。
inline constexpr std::size_t kImProvisioningHeaderSize = 12;

/// 已校验的 provisioning 字段长度。
struct ImProvisioningHeader {
    /// Gateway origin 的 UTF-8 字节数。
    std::size_t gateway_origin_size = 0;
    /// 设备 ID 的字节数。
    std::size_t device_id_size = 0;
    /// Bearer Token 的字节数。
    std::size_t device_token_size = 0;
    /// 可选用户引用的字节数。
    std::size_t user_id_size = 0;
    /// 固定头之后需要读取的总字节数。
    std::size_t payload_size = 0;
};

/// 从受控本地 provisioning 帧得到的 IM 配置。
struct ImProvisioningRequest {
    /// Gateway HTTPS origin。
    std::string gateway_origin;
    /// Gateway 设备身份。
    std::string device_id;
    /// Gateway Bearer Token；调用者落盘后应尽快清零。
    std::string device_token;
    /// 可选的非 Secret 用户引用。
    std::string user_id;
};

/**
 * @brief 校验 VLI1 provisioning 固定头及各字段长度。
 * @param bytes 至少包含 12 字节固定头的输入。
 * @return 已校验字段长度，或类型化协议错误。
 */
Result<ImProvisioningHeader> ParseImProvisioningHeader(std::span<const uint8_t> bytes);

/**
 * @brief 严格解析一帧完整 VLI1 provisioning 请求。
 * @param bytes 固定头和长度完全相符的单帧输入。
 * @return 解析后的配置；magic、长度或内容异常时 fail closed。
 */
Result<ImProvisioningRequest> ParseImProvisioningRequest(std::span<const uint8_t> bytes);

}  // namespace voicelife::im
