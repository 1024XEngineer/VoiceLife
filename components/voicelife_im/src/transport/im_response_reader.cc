#include "im_response_reader.h"

#include <algorithm>
#include <cstddef>

namespace voicelife::im {

bool ReadResponseBody(ImResponseReader& reader, std::string& body, size_t max_bytes) {
    const int64_t content_length = reader.ContentLength();
    char buffer[256];
    int64_t remaining = content_length;
    while (remaining != 0 && body.size() < max_bytes) {
        // 每次最多读入剩余容量，避免单次读取越过上限。
        const size_t want = std::min(sizeof(buffer), max_bytes - body.size());
        const int n = reader.Read(buffer, want);
        if (n < 0) {
            // 网络/TLS 读取错误：响应不完整，body 不可信，不得按成功处理。
            return false;
        }
        if (n == 0) {
            // EOF：已知长度未读满属提前结束（截断）；分块流读到 EOF 属正常结束。
            return content_length < 0;
        }
        body.append(buffer, static_cast<size_t>(n));
        if (remaining > 0) {
            remaining -= n;
        }
    }
    // 命中上限仍有数据待读视为截断；已知长度恰好读满属完整。
    return remaining == 0;
}

}  // namespace voicelife::im
