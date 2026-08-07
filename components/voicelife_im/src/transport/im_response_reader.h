#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace voicelife::im {

/// 响应体字节读取源。封装 esp_http_client_read 等底层读取，
/// 使读取完整性判定可在主机侧用假源回归测试。
class ImResponseReader {
   public:
    virtual ~ImResponseReader() = default;
    /// 响应体声明长度（字节）；-1 表示分块编码/长度未知。
    virtual int64_t ContentLength() const = 0;
    /// 读取至多 size 字节到 buffer；返回实际读取字节数（>0）、0（EOF）或负数（网络错误）。
    virtual int Read(char* buffer, size_t size) = 0;
};

/// 把响应体读到 body，读取总量受 max_bytes 限制。
/// 返回 true 表示完整读取：已知长度读满，或分块流读到 EOF；
/// 返回 false 表示提前 EOF、读取错误或超限截断，此时 body 不完整，
/// 调用方不得按成功受理处理。
bool ReadResponseBody(ImResponseReader& reader, std::string& body, size_t max_bytes);

}  // namespace voicelife::im
