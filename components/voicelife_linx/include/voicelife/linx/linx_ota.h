#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "voicelife/contracts/status.h"
#include "voicelife/linx/linx_types.h"

namespace voicelife::linx {

/** Linx XRobot OTA 网关的固定 HTTPS 地址。 */
inline constexpr char kLinxOtaUrl[] = "https://xrobo.qiniuapi.com/v1/ota/";

/** 描述一项 HTTP 请求头。 */
struct LinxOtaHttpHeader {
    /// HTTP 头名称。
    std::string name;
    /// HTTP 头值。
    std::string value;
};

/** 描述提交到 Linx OTA 网关的 HTTP 请求。 */
struct LinxOtaHttpRequest {
    /// 固定为 Linx OTA 网关地址。
    std::string url = kLinxOtaUrl;
    /// 固定为 POST。
    std::string method = "POST";
    /// 认证与设备身份请求头，不包含 WebSocket token。
    std::vector<LinxOtaHttpHeader> headers;
    /// 已序列化的设备元数据 JSON 请求体。
    std::string body;
};

/** 描述发起 OTA 请求所需的脱敏设备元数据。 */
struct LinxOtaDeviceInfo {
    /// 激活协议版本，只接受 "1" 或 "2"。
    std::string activation_version;
    /// 设备物理标识，同时必须等于 mac_address。
    std::string device_id;
    /// 本次安装生成的客户端 UUID。
    std::string client_id;
    /// OTA 请求 User-Agent，例如 voicelife-pcb/0.1.0。
    std::string user_agent;
    /// 当前固件名称。
    std::string application_name;
    /// 当前固件版本。
    std::string application_version;
    /// 当前 ELF 的 SHA-256。
    std::string application_elf_sha256;
    /// Linx 设备型号，例如 esp32s3。
    std::string chip_model_name;
    /// Flash 容量，单位 byte。
    uint32_t flash_size_bytes = 0;
    /// PSRAM 容量，单位 byte；0 表示不可用。
    uint32_t psram_size_bytes = 0;
    /// 分区表 JSON 数组，必须是有效 JSON array。
    std::string partition_table_json;
    /// 板型类别。
    std::string board_type;
    /// 板型 SKU，必须与 User-Agent 的产品名一致。
    std::string board_name;
    /// 当前 STA SSID；仅用于 OTA 请求，调用者不得记录到日志。
    std::string wifi_ssid;
    /// 当前 STA RSSI，单位 dBm。
    int wifi_rssi = 0;
    /// 可选的出厂序列号。
    std::optional<std::string> serial_number;
    /// 可选的界面语言，例如 zh-CN。
    std::optional<std::string> accept_language;
};

/** 描述 OTA 响应中的设备激活信息。 */
struct LinxActivation {
    /// 控制台绑定设备时展示的激活码。
    std::string code;
    /// 可向用户展示的服务端消息。
    std::string message;
    /// 可选的服务端挑战值。
    std::optional<std::string> challenge;
};

/** 描述 OTA 响应中的 WebSocket 配置。 */
struct LinxOtaWebSocket {
    /// 服务端下发的 WebSocket 地址。
    std::string url;
    /// 服务端明确下发时才保存的 token；文档示例未保证该字段存在。
    std::optional<std::string> token;
};

/** 描述 OTA 响应中的服务端时间。 */
struct LinxServerTime {
    /// Unix epoch 毫秒。
    uint64_t timestamp_ms = 0;
    /// 可选时区名称。
    std::optional<std::string> timezone;
    /// 可选时区偏移，单位分钟。
    std::optional<int32_t> timezone_offset_minutes;
};

/** 描述 OTA 响应中的可用固件更新。 */
struct LinxFirmwareUpdate {
    /// 可用固件版本。
    std::string version;
    /// 可选的固件下载地址。
    std::optional<std::string> url;
};

/** 描述经类型校验后的 Linx OTA 响应。 */
struct LinxOtaResponse {
    /// 设备是否需要控制台激活。
    std::optional<LinxActivation> activation;
    /// 可选 WebSocket 连接配置。
    std::optional<LinxOtaWebSocket> websocket;
    /// 可选服务端时间。
    std::optional<LinxServerTime> server_time;
    /// 可选固件更新描述。
    std::optional<LinxFirmwareUpdate> firmware;
};

/**
 * @brief 根据设备事实构建 Linx OTA POST 请求。
 * @param device 已验证的设备、固件、板型和 Wi-Fi 元数据。
 * @return 完整请求或参数校验错误。
 */
Result<LinxOtaHttpRequest> BuildLinxOtaRequest(const LinxOtaDeviceInfo& device);

/**
 * @brief 解析 Linx OTA 成功响应。
 * @param body 网关返回的完整 JSON 响应体。
 * @return 已校验的响应，或协议字段错误。
 */
Result<LinxOtaResponse> ParseLinxOtaResponse(std::string_view body);

/**
 * @brief 将包含明确 token 的 OTA 响应转换为 WSS 连接配置。
 * @param response 已通过 JSON 类型校验的 OTA 响应。
 * @param device_id 用于 WSS 鉴权的设备标识。
 * @param client_id 用于 WSS 鉴权的客户端标识。
 * @param token_reference 指向平台安全存储中已写入 token 的引用。
 * @return 可供 Transport 使用的配置，或缺少 token/WSS 地址的错误。
 */
Result<LinxConnectionConfig> BuildLinxConnectionConfig(const LinxOtaResponse& response, std::string_view device_id,
                                                       std::string_view client_id, std::string_view token_reference);

}  // namespace voicelife::linx
