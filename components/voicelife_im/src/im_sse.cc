#include "voicelife/im/im_sse.h"

#include <utility>

namespace voicelife::im {
namespace {

// 去除 SSE 字段值的首尾空白（id/event 规范要求去除尾部空白，首部单空格由
// ": " 写法产生，统一去除）。
std::string Trim(std::string value) {
    const size_t first = value.find_first_not_of(" \t");
    if (first == std::string::npos) {
        return "";
    }
    const size_t last = value.find_last_not_of(" \t");
    return value.substr(first, last - first + 1);
}

}  // namespace

void SseDecoder::Feed(std::string_view bytes, std::vector<SseFrame>& frames) {
    // 归一化 CRLF 与孤立 CR 为 LF，保证按 '\n' 解析行；\r\n 必须折叠为单个 '\n'，
    // 否则帧会被空行判定提前切开。\r 可能落在本次喂入末尾而 \n 落在下次喂入开头，
    // 用 trailing_cr_ 记住待折叠的 CR，避免两处拼出虚假的空行边界。
    for (size_t i = 0; i < bytes.size(); ++i) {
        if (bytes[i] == '\r') {
            buffer_.push_back('\n');
            trailing_cr_ = true;
            if (i + 1 < bytes.size() && bytes[i + 1] == '\n') {
                ++i;
                trailing_cr_ = false;
            }
            continue;
        }
        if (bytes[i] == '\n' && trailing_cr_) {
            trailing_cr_ = false;
            continue;
        }
        trailing_cr_ = false;
        buffer_.push_back(bytes[i]);
    }

    // 以空行（连续两个 LF）切出完整帧块。
    while (true) {
        const size_t separator = buffer_.find("\n\n");
        if (separator == std::string::npos) {
            break;
        }
        std::string block = buffer_.substr(0, separator);
        buffer_.erase(0, separator + 2);
        SseFrame frame;
        if (ParseBlock(block, frame)) {
            frames.push_back(std::move(frame));
        }
    }
}

void SseDecoder::Reset() {
    buffer_.clear();
    trailing_cr_ = false;
}

bool SseDecoder::ParseBlock(const std::string& block, SseFrame& frame) {
    std::string data;
    bool present = false;
    size_t line_start = 0;
    while (line_start <= block.size()) {
        const size_t newline = block.find('\n', line_start);
        const std::string line =
            block.substr(line_start, newline == std::string::npos ? std::string::npos : newline - line_start);
        line_start = newline == std::string::npos ? block.size() + 1 : newline + 1;

        if (line.empty() || line.front() == ':') {
            // 空行或心跳注释帧不携带业务字段。
            continue;
        }
        const size_t colon = line.find(':');
        if (colon == std::string::npos) {
            // 无字段名的行按规范忽略。
            continue;
        }
        std::string value = line.substr(colon + 1);
        if (!value.empty() && value.front() == ' ') {
            value.erase(0, 1);
        }
        const std::string field = line.substr(0, colon);
        if (field == "id") {
            frame.id = Trim(std::move(value));
            present = true;
        } else if (field == "event") {
            frame.event = Trim(std::move(value));
            present = true;
        } else if (field == "data") {
            if (!data.empty()) {
                data.push_back('\n');
            }
            data += value;
            present = true;
        }
    }
    if (!present) {
        return false;
    }
    frame.data = std::move(data);
    return true;
}

}  // namespace voicelife::im
