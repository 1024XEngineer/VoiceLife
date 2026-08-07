// #127 动作通道 HTTP 受理响应读取：完整性判定回归（TDD 先写）。
// 验收来源：codex 审阅 —— 已知长度提前 EOF、读取错误不得误判为成功受理，
// 只有读满或分块流读到 EOF 才算完整。

#include "im_response_reader.h"

#include <string>
#include <vector>

#include "support/test_support.h"

using voicelife::test::Check;

namespace {
using namespace voicelife::im;

/// 可编排读取序列的假响应源。outcomes 中的每个正数产生对应长度字节，
/// 0 表示 EOF，负数表示网络错误；耗尽后默认 EOF。
class FakeReader : public ImResponseReader {
   public:
    int64_t content_length = -1;
    std::vector<int> outcomes;
    size_t index = 0;

    int64_t ContentLength() const override { return content_length; }
    int Read(char* buffer, size_t size) override {
        if (index >= outcomes.size()) {
            return 0;
        }
        int n = outcomes[index++];
        if (n > 0) {
            if (static_cast<size_t>(n) > size) {
                n = static_cast<int>(size);
            }
            for (int i = 0; i < n; ++i) {
                buffer[i] = 'x';
            }
        }
        return n;
    }
};

/// 便捷封装：跑一轮读取并返回完整性判定。
bool ReadInto(FakeReader& reader, std::string& body, size_t max_bytes = 64 * 1024) {
    return ReadResponseBody(reader, body, max_bytes);
}

void TestKnownLengthFullReadIsComplete() {
    FakeReader reader;
    reader.content_length = 10;
    reader.outcomes = {10};
    std::string body;
    Check(ReadInto(reader, body), "已知长度读满必须判定为完整");
    Check(body.size() == 10, "读满的 body 长度必须等于 Content-Length");
}

void TestKnownLengthPrematureEofIsIncomplete() {
    FakeReader reader;
    reader.content_length = 10;
    reader.outcomes = {5, 0};  // 只读到一半便 EOF
    std::string body;
    Check(!ReadInto(reader, body), "已知长度未读满的 EOF 必须判定为不完整");
    Check(body.size() == 5, "提前 EOF 的 body 只能包含已读部分");
}

void TestReadErrorIsIncomplete() {
    FakeReader reader;
    reader.content_length = 10;
    reader.outcomes = {-1};  // 首次读取即网络错误
    std::string body;
    Check(!ReadInto(reader, body), "读取返回负数必须判定为不完整");
}

void TestChunkedEofIsComplete() {
    FakeReader reader;
    reader.content_length = -1;
    reader.outcomes = {5, 5, 0};  // 分块流正常读到 EOF
    std::string body;
    Check(ReadInto(reader, body), "分块流读到 EOF 必须判定为完整");
    Check(body.size() == 10, "分块流完整读取的 body 必须累积全部字节");
}

void TestChunkedReadErrorIsIncomplete() {
    FakeReader reader;
    reader.content_length = -1;
    reader.outcomes = {5, -1};  // 分块流中途读取错误
    std::string body;
    Check(!ReadInto(reader, body), "分块流读取错误必须判定为不完整");
}

void TestTruncatedOverLimitIsIncomplete() {
    FakeReader reader;
    reader.content_length = 1000;
    reader.outcomes = {100};  // 首读即超过 max_bytes 上限
    std::string body;
    Check(!ReadInto(reader, body, 64), "命中上限仍有数据待读必须判定为截断不完整");
    Check(body.size() == 64, "截断的 body 不得超过上限");
}

void TestEmptyBodyIsComplete() {
    FakeReader reader;
    reader.content_length = 0;
    std::string body;
    Check(ReadInto(reader, body), "Content-Length 为 0 必须判定为完整");
    Check(body.empty(), "空响应体不得包含字节");
}

void TestExactFitAtLimitIsComplete() {
    FakeReader reader;
    reader.content_length = 64;
    reader.outcomes = {64};  // 恰好读满上限
    std::string body;
    Check(ReadInto(reader, body, 64), "Content-Length 恰好等于上限且读满必须判定为完整");
    Check(body.size() == 64, "恰好读满的 body 长度必须等于上限");
}

}  // namespace

int main() {
    TestKnownLengthFullReadIsComplete();
    TestKnownLengthPrematureEofIsIncomplete();
    TestReadErrorIsIncomplete();
    TestChunkedEofIsComplete();
    TestChunkedReadErrorIsIncomplete();
    TestTruncatedOverLimitIsIncomplete();
    TestEmptyBodyIsComplete();
    TestExactFitAtLimitIsComplete();
    return 0;
}
