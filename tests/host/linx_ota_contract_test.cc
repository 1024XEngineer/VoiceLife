#include <string>

#include "support/test_support.h"
#include "voicelife/contracts/json.h"
#include "voicelife/linx/linx_ota.h"

using voicelife::ErrorCode;
using voicelife::JsonValue;
using voicelife::linx::BuildLinxConnectionConfig;
using voicelife::linx::BuildLinxOtaRequest;
using voicelife::linx::LinxOtaDeviceInfo;
using voicelife::linx::ParseLinxOtaResponse;
using voicelife::test::Check;

namespace {

LinxOtaDeviceInfo DeviceInfo() {
    return {.activation_version = "1",
            .device_id = "98:a3:16:e6:91:dc",
            .client_id = "1b31b374-196e-4798-ab72-b41a8bb12bad",
            .user_agent = "voicelife-pcb/0.1.0",
            .application_name = "voicelife-pcb",
            .application_version = "0.1.0",
            .application_elf_sha256 = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
            .chip_model_name = "esp32s3",
            .flash_size_bytes = 16U * 1024U * 1024U,
            .psram_size_bytes = 8U * 1024U * 1024U,
            .partition_table_json = R"([{"label":"ota_0","type":0,"subtype":16,"address":65536,"size":4194304}])",
            .board_type = "voicelife-pcb",
            .board_name = "voicelife-pcb",
            .wifi_ssid = "test-network",
            .wifi_rssi = -48,
            .serial_number = std::nullopt,
            .accept_language = std::string("zh-CN")};
}

std::string HeaderValue(const voicelife::linx::LinxOtaHttpRequest& request, const std::string& name) {
    for (const auto& header : request.headers) {
        if (header.name == name) return header.value;
    }
    return {};
}

}  // namespace

int main() {
    const auto built = BuildLinxOtaRequest(DeviceInfo());
    Check(built.ok() && built.value.has_value(), "完整板级信息应构建 Linx OTA 请求");
    Check(built.value->url == "https://xrobo.qiniuapi.com/v1/ota/" && built.value->method == "POST",
          "OTA 请求必须使用官方 HTTPS 地址和 POST");
    Check(HeaderValue(*built.value, "Activation-Version") == "1" &&
              HeaderValue(*built.value, "Device-Id") == "98:a3:16:e6:91:dc" &&
              HeaderValue(*built.value, "Client-Id") == "1b31b374-196e-4798-ab72-b41a8bb12bad" &&
              HeaderValue(*built.value, "Accept-Language") == "zh-CN",
          "OTA 请求必须携带协议要求的设备身份头");

    JsonValue body;
    Check(voicelife::ParseJson(built.value->body, body).ok() && body.IsObject(), "OTA 请求体必须是合法 JSON 对象");
    Check(body.Get("mac_address")->string == "98:a3:16:e6:91:dc" &&
              body.Get("uuid")->string == "1b31b374-196e-4798-ab72-b41a8bb12bad" &&
              body.Get("application")->Get("version")->string == "0.1.0" &&
              body.Get("board")->Get("ssid")->string == "test-network" &&
              body.Get("board")->Get("rssi")->number == -48 && body.Get("partition_table")->array.size() == 1 &&
              body.Get("psram_size")->number == 8U * 1024U * 1024U,
          "OTA 请求体必须保留设备、Wi-Fi 和完整分区表事实");

    auto invalid = DeviceInfo();
    invalid.device_id = "invalid\r\nheader";
    Check(BuildLinxOtaRequest(invalid).status.code == ErrorCode::kInvalidArgument,
          "OTA 请求必须拒绝注入 HTTP 头的设备身份");
    invalid = DeviceInfo();
    invalid.partition_table_json = "{}";
    Check(BuildLinxOtaRequest(invalid).status.code == ErrorCode::kInvalidArgument, "OTA 请求必须拒绝非数组分区表");
    invalid = DeviceInfo();
    invalid.accept_language = std::string("zh-CN\r\nX-Leak: value");
    Check(BuildLinxOtaRequest(invalid).status.code == ErrorCode::kInvalidArgument,
          "OTA 可选请求头必须拒绝控制字符注入");

    const auto parsed = ParseLinxOtaResponse(R"({
        "server_time":{"timestamp":1752119934489,"timeZone":"Asia/Shanghai","timezone_offset":480},
        "activation":{"code":"608303","message":"activate","challenge":"98:a3:16:e6:91:dc"},
        "firmware":{"version":"1.0.0","url":"https://xrobo.qiniuapi.com/v1/ota/firmware"},
        "websocket":{"url":"wss://xrobo-io.qiniuapi.com/v1/ws/","token":"device-token"}
    })");
    Check(parsed.ok() && parsed.value.has_value() && parsed.value->activation->code == "608303" &&
              parsed.value->websocket->url == "wss://xrobo-io.qiniuapi.com/v1/ws/" &&
              parsed.value->websocket->token == std::optional<std::string>("device-token") &&
              parsed.value->server_time->timestamp_ms == 1752119934489ULL &&
              parsed.value->server_time->timezone == std::optional<std::string>("Asia/Shanghai") &&
              parsed.value->firmware->version == "1.0.0",
          "OTA 响应必须解析文档化的激活、WSS、时间和固件字段");
    const auto config = BuildLinxConnectionConfig(*parsed.value, "98:a3:16:e6:91:dc",
                                                  "1b31b374-196e-4798-ab72-b41a8bb12bad", "nvs://linx/token");
    Check(config.ok() && config.value->valid() && config.value->token_ref == "nvs://linx/token",
          "只有已保存 token 的 OTA 响应才能生成 WSS 连接配置");

    const auto url_only = ParseLinxOtaResponse(R"({"websocket":{"url":"ws://xrobo-io.qiniuapi.com/v1/ws/"}})");
    Check(url_only.ok() && url_only.value->websocket.has_value() && !url_only.value->websocket->token.has_value(),
          "文档示例的无 token WebSocket 响应必须可表达，不能伪造凭据");
    Check(BuildLinxConnectionConfig(*url_only.value, "device", "client", "nvs://linx/token").status.code ==
              ErrorCode::kInvalidArgument,
          "无 token 或非 WSS OTA 响应不得生成可连接配置");
    const auto ws_with_token =
        ParseLinxOtaResponse(R"({"websocket":{"url":"ws://xrobo-io.qiniuapi.com/v1/ws/","token":"device-token"}})");
    const auto upgraded = BuildLinxConnectionConfig(*ws_with_token.value, "device", "client", "nvs://linx/token");
    Check(upgraded.ok() && upgraded.value->websocket_url == "wss://xrobo-io.qiniuapi.com/v1/ws/",
          "官方 OTA 示例的 WS 地址必须升级为 WSS 后才允许连接");
    const auto untrusted_ws =
        ParseLinxOtaResponse(R"({"websocket":{"url":"ws://untrusted.example/v1/ws/","token":"device-token"}})");
    Check(BuildLinxConnectionConfig(*untrusted_ws.value, "device", "client", "nvs://linx/token").status.code ==
              ErrorCode::kInvalidArgument,
          "非官方明文 WebSocket 地址必须拒绝");
    Check(ParseLinxOtaResponse(R"({"websocket":{"url":42}})").status.code == ErrorCode::kInvalidArgument,
          "OTA 响应中错误类型的 WSS 地址必须拒绝");
    Check(ParseLinxOtaResponse(R"({"error":"Invalid OTA request"})").status.code == ErrorCode::kUnavailable,
          "OTA 服务错误必须映射为可诊断失败");
    return 0;
}
