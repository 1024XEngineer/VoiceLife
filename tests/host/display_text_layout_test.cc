#include "display_text_layout.h"

#include <string_view>

#include "support/test_support.h"

using voicelife::display_esp::text_layout::Advance16;
using voicelife::display_esp::text_layout::ByteOffsetAfterCodepoints;
using voicelife::display_esp::text_layout::DecodeFirst;
using voicelife::test::Check;

int main() {
    Check(DecodeFirst("A").codepoint == 'A' && DecodeFirst("A").byte_width == 1, "ASCII 必须保持单字节和原码点");
    const auto chinese = DecodeFirst("牛");
    Check(chinese.codepoint == 0x725bU && chinese.byte_width == 3, "中文必须保留三字节 UTF-8 码点");
    const auto full_width = DecodeFirst("！");
    Check(full_width.codepoint == '!' && full_width.byte_width == 3, "全角 ASCII 标点必须归一化为半角码点");
    const auto emoji = DecodeFirst("😀");
    Check(emoji.codepoint == 0x1f600U && emoji.byte_width == 4, "四字节 UTF-8 必须完整解码");
    const auto truncated = DecodeFirst(std::string_view("\xe7\x89", 2));
    Check(truncated.codepoint == 0 && truncated.byte_width == 1, "截断 UTF-8 必须按一个无效字节前进");

    Check(Advance16('A') == 9 && Advance16(0x725bU) == 17 && Advance16(0x3002U) == 17,
          "16px 布局必须保留半角和 CJK 的既有前进宽度");
    const std::string_view content = "A牛B😀";
    Check(ByteOffsetAfterCodepoints(content, 0) == 0, "零滚动偏移必须从文本起点开始");
    Check(ByteOffsetAfterCodepoints(content, 1) == 1, "ASCII 必须按一个字符而非像素跳过");
    Check(ByteOffsetAfterCodepoints(content, 2) == 4, "中文必须作为一个滚动字符跳过");
    Check(ByteOffsetAfterCodepoints(content, 3) == 5, "混合文本的第三个字符应为 ASCII");
    Check(ByteOffsetAfterCodepoints(content, 4) == content.size(), "四字节字符必须整体跳过");
    return 0;
}
