#include <array>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <set>
#include <string>
#include <string_view>

#include "support/test_support.h"
#include "voicelife/display_sparkbot/sparkbot_emoji_assets.h"
#include "yyjson.h"

using voicelife::test::Check;

namespace {

// 紧凑 SHA-256（RFC 6234），仅用于主机测试校验资源哈希，不进入产品代码。
class Sha256 {
   public:
    static constexpr std::size_t kDigestSize = 32;

    void Update(const std::string& data) {
        for (char c : data) {
            buf_[buf_len_++] = static_cast<uint8_t>(c);
            if (buf_len_ == 64) {
                ProcessBlock(buf_);
                total_len_ += 64;
                buf_len_ = 0;
            }
        }
    }

    std::array<uint8_t, kDigestSize> Final() {
        const uint64_t bit_len = (total_len_ + buf_len_) * 8;
        buf_[buf_len_++] = 0x80;
        if (buf_len_ > 56) {
            std::fill(buf_.begin() + buf_len_, buf_.end(), 0);
            ProcessBlock(buf_);
            buf_len_ = 0;
        }
        std::fill(buf_.begin() + buf_len_, buf_.begin() + 56, 0);
        for (int i = 0; i < 8; ++i) {
            buf_[56 + i] = static_cast<uint8_t>((bit_len >> (56 - 8 * i)) & 0xff);
        }
        ProcessBlock(buf_);
        std::array<uint8_t, kDigestSize> out{};
        for (int i = 0; i < 8; ++i) {
            out[4 * i] = static_cast<uint8_t>(h_[i] >> 24);
            out[4 * i + 1] = static_cast<uint8_t>((h_[i] >> 16) & 0xff);
            out[4 * i + 2] = static_cast<uint8_t>((h_[i] >> 8) & 0xff);
            out[4 * i + 3] = static_cast<uint8_t>(h_[i] & 0xff);
        }
        return out;
    }

   private:
    static uint32_t Rotr(uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); }

    void ProcessBlock(const std::array<uint8_t, 64>& block) {
        static constexpr std::array<uint32_t, 64> kK = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

        std::array<uint32_t, 64> w{};
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<uint32_t>(block[4 * i]) << 24) | (static_cast<uint32_t>(block[4 * i + 1]) << 16) |
                   (static_cast<uint32_t>(block[4 * i + 2]) << 8) | static_cast<uint32_t>(block[4 * i + 3]);
        }
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        uint32_t a = h_[0];
        uint32_t b = h_[1];
        uint32_t c = h_[2];
        uint32_t d = h_[3];
        uint32_t e = h_[4];
        uint32_t f = h_[5];
        uint32_t g = h_[6];
        uint32_t h = h_[7];
        for (int i = 0; i < 64; ++i) {
            const uint32_t s1 = Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25);
            const uint32_t ch = (e & f) ^ (~e & g);
            const uint32_t t1 = h + s1 + ch + kK[i] + w[i];
            const uint32_t s0 = Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22);
            const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t t2 = s0 + maj;
            h = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h_[0] += a;
        h_[1] += b;
        h_[2] += c;
        h_[3] += d;
        h_[4] += e;
        h_[5] += f;
        h_[6] += g;
        h_[7] += h;
    }

    std::array<uint32_t, 8> h_{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                               0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::array<uint8_t, 64> buf_{};
    std::size_t buf_len_ = 0;
    uint64_t total_len_ = 0;
};

std::string ToHex(const std::array<uint8_t, Sha256::kDigestSize>& digest) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (uint8_t b : digest) {
        out.push_back(kHex[b >> 4]);
        out.push_back(kHex[b & 0xf]);
    }
    return out;
}

std::string ReadFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

}  // namespace

int main() {
    const std::string asset_dir =
        std::string(VOICELIFE_SOURCE_DIR) + "/components/voicelife_display_esp/assets/esp-sparkbot";
    const std::string manifest_path = asset_dir + "/manifest.json";
    const std::string manifest_text = ReadFile(manifest_path);
    Check(!manifest_text.empty(), "manifest.json 必须存在且可读");

    yyjson_doc* doc = yyjson_read(manifest_text.data(), manifest_text.size(), YYJSON_READ_NOFLAG);
    Check(doc != nullptr, "manifest.json 必须是合法 JSON");
    yyjson_val* root = yyjson_doc_get_root(doc);
    Check(root != nullptr && yyjson_is_obj(root), "manifest.json 根节点必须是对象");

    Check(yyjson_get_int(yyjson_obj_get(root, "schema_version")) == 1, "schema_version 必须为 1");
    Check(yyjson_equals_str(yyjson_obj_get(root, "board_id"), "esp-sparkbot"), "board_id 必须为 esp-sparkbot");

    // 加载约束：资源只经 manifest 约束的 asset_id 使用，禁止 URL/任意路径/任意字节流。
    yyjson_val* loading = yyjson_obj_get(root, "loading");
    Check(loading != nullptr && yyjson_is_obj(loading), "loading 约束必须存在");
    Check(yyjson_equals_str(yyjson_obj_get(loading, "mode"), "embedded_manifest_only"),
          "资源加载模式必须为 embedded_manifest_only");
    Check(yyjson_get_bool(yyjson_obj_get(loading, "allow_network_url")) == false, "不允许通过网络 URL 加载资源");
    Check(yyjson_get_bool(yyjson_obj_get(loading, "allow_arbitrary_path")) == false, "不允许通过任意路径加载资源");
    Check(yyjson_get_bool(yyjson_obj_get(loading, "allow_arbitrary_bytes")) == false, "不允许加载任意字节流");

    // 来源与许可证记录必须完整。
    yyjson_val* source = yyjson_obj_get(root, "source");
    Check(source != nullptr && yyjson_is_obj(source), "来源记录必须存在");
    Check(yyjson_is_str(yyjson_obj_get(source, "upstream_commit")) &&
              yyjson_get_len(yyjson_obj_get(source, "upstream_commit")) > 0,
          "上游 commit 必须记录");
    Check(yyjson_is_str(yyjson_obj_get(source, "license")) && yyjson_get_len(yyjson_obj_get(source, "license")) > 0,
          "许可证必须记录");
    Check(yyjson_equals_str(yyjson_obj_get(source, "license"), "MIT"), "素材许可证必须为 MIT");

    yyjson_val* budget = yyjson_obj_get(root, "budget");
    Check(budget != nullptr && yyjson_is_obj(budget), "内存预算必须存在");

    yyjson_val* assets = yyjson_obj_get(root, "assets");
    Check(assets != nullptr && yyjson_is_arr(assets), "assets 必须是数组");
    const std::size_t asset_count = yyjson_arr_size(assets);
    Check(asset_count == 10, "SparkBot 牛头素材清单必须恰好包含 10 个 GIF");

    std::set<std::string> asset_ids;
    std::uint64_t total_bytes = 0;
    for (std::size_t i = 0; i < asset_count; ++i) {
        yyjson_val* asset = yyjson_arr_get(assets, i);
        Check(asset != nullptr && yyjson_is_obj(asset), "每个资源条目必须是对象");

        const char* asset_id = yyjson_get_str(yyjson_obj_get(asset, "asset_id"));
        const char* file = yyjson_get_str(yyjson_obj_get(asset, "file"));
        const char* sha = yyjson_get_str(yyjson_obj_get(asset, "sha256"));
        Check(asset_id != nullptr && file != nullptr && sha != nullptr, "asset_id/file/sha256 字段必须齐全");
        Check(asset_ids.insert(asset_id).second, "asset_id 必须唯一");

        // asset_id 必须是受控标识：非空、不含路径分隔符或 ..
        const std::string asset_id_str(asset_id);
        Check(!asset_id_str.empty() && asset_id_str.find('/') == std::string::npos &&
                  asset_id_str.find('\\') == std::string::npos && asset_id_str.find("..") == std::string::npos &&
                  asset_id_str != "." && asset_id_str != "..",
              "asset_id 必须是单段受控标识，不得含路径特征");

        // 条目只允许清单 schema 固定键；禁止 url/path/remote 等未知加载字段。
        {
            static constexpr const char* kAllowedKeys[] = {"asset_id",    "file",        "width",      "height",
                                                           "frame_count", "duration_ms", "size_bytes", "sha256"};
            std::size_t key_count = 0;
            std::size_t idx = 0;
            std::size_t max = 0;
            yyjson_val* key = nullptr;
            yyjson_val* value = nullptr;
            yyjson_obj_foreach(asset, idx, max, key, value) {
                ++key_count;
                bool allowed = false;
                for (const char* k : kAllowedKeys) {
                    if (yyjson_equals_str(key, k)) {
                        allowed = true;
                        break;
                    }
                }
                Check(allowed, "资源条目不得携带未知字段（含 url/path 等加载字段）");
            }
            Check(key_count == sizeof(kAllowedKeys) / sizeof(kAllowedKeys[0]),
                  "资源条目键集合必须与清单 schema 完全一致");
        }

        // file 只能是单文件名：无路径分隔符、非相对路径跳转、仅 .gif。
        const std::string file_str(file);
        Check(!file_str.empty() && file_str.find('/') == std::string::npos &&
                  file_str.find('\\') == std::string::npos && file_str != "." && file_str != ".." &&
                  file_str.size() >= 4 && file_str.compare(file_str.size() - 4, 4, ".gif") == 0,
              "file 必须是资源目录内的单个 .gif 文件名，不得含路径");
        const std::string asset_file_path = asset_dir + "/mascot/gifs/" + file_str;
        Check(asset_file_path.rfind(asset_dir, 0) == 0, "拼接后的资源路径必须仍位于固定资源目录内（canonical 约束）");

        // 数值字段必须存在、类型正确且为正。
        Check(yyjson_is_int(yyjson_obj_get(asset, "width")) && yyjson_get_int(yyjson_obj_get(asset, "width")) > 0 &&
                  yyjson_is_int(yyjson_obj_get(asset, "height")) &&
                  yyjson_get_int(yyjson_obj_get(asset, "height")) > 0 &&
                  yyjson_is_int(yyjson_obj_get(asset, "frame_count")) &&
                  yyjson_get_int(yyjson_obj_get(asset, "frame_count")) >= 1 &&
                  yyjson_is_int(yyjson_obj_get(asset, "duration_ms")) &&
                  yyjson_get_int(yyjson_obj_get(asset, "duration_ms")) > 0,
              "width/height/frame_count/duration_ms 必须存在、类型正确且为正");
        Check(yyjson_is_uint(yyjson_obj_get(asset, "size_bytes")) &&
                  yyjson_get_uint(yyjson_obj_get(asset, "size_bytes")) > 0,
              "size_bytes 必须是正数");

        // SHA-256 必须是严格 64 位小写十六进制。
        {
            const std::string sha_str(sha);
            Check(sha_str.size() == 64, "SHA-256 必须为 64 位十六进制");
            for (char c : sha_str) {
                const bool is_hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
                Check(is_hex, "SHA-256 只允许小写十六进制字符");
            }
        }

        const std::string file_text = ReadFile(asset_file_path);
        Check(!file_text.empty(), "清单引用的 GIF 文件必须存在");

        // GIF 头与尺寸必须符合预期。
        Check(file_text.size() >= 10 && file_text.compare(0, 6, "GIF89a") == 0, "素材必须是 GIF89a 格式");
        const std::uint16_t width = static_cast<std::uint16_t>(static_cast<std::uint8_t>(file_text[6])) |
                                    (static_cast<std::uint16_t>(static_cast<std::uint8_t>(file_text[7])) << 8);
        const std::uint16_t height = static_cast<std::uint16_t>(static_cast<std::uint8_t>(file_text[8])) |
                                     (static_cast<std::uint16_t>(static_cast<std::uint8_t>(file_text[9])) << 8);
        Check(yyjson_get_int(yyjson_obj_get(asset, "width")) == width &&
                  yyjson_get_int(yyjson_obj_get(asset, "height")) == height,
              "manifest 尺寸必须与实际 GIF 头一致");
        Check(width == 96 && height == 96, "SparkBot 牛头素材必须为 96x96");

        const std::uint64_t size_bytes = file_text.size();
        Check(yyjson_get_uint(yyjson_obj_get(asset, "size_bytes")) == size_bytes,
              "manifest size_bytes 必须与实际文件大小一致");
        total_bytes += size_bytes;

        Sha256 hasher;
        hasher.Update(file_text);
        Check(ToHex(hasher.Final()) == sha, "文件 SHA-256 必须与 manifest 记录一致");
    }

    Check(total_bytes == 142683, "GIF 总大小必须与已验证的 142683 字节一致");
    Check(yyjson_get_uint(yyjson_obj_get(budget, "gif_bytes")) == total_bytes,
          "budget.gif_bytes 必须等于全部 GIF 字节数之和");

    // 字体是固定受控资源，不进入 Runtime 可传入的 GIF asset_id 集合。
    yyjson_val* text_font = yyjson_obj_get(root, "text_font");
    Check(text_font != nullptr && yyjson_is_obj(text_font), "common 文本字体声明必须存在");
    Check(yyjson_equals_str(yyjson_obj_get(text_font, "file"), "font_noto_sans_common_14_1.bin"),
          "文本字体必须固定为官方 common 14px 文件");
    Check(yyjson_get_int(yyjson_obj_get(text_font, "size_px")) == 14 &&
              yyjson_get_int(yyjson_obj_get(text_font, "bpp")) == 1 &&
              yyjson_get_int(yyjson_obj_get(text_font, "line_height")) == 16 &&
              yyjson_get_int(yyjson_obj_get(text_font, "base_line")) == 2,
          "common 文本字体必须保持官方 14px/1bpp/line_height=16/base_line=2");
    const std::string font_file_path = asset_dir + "/fonts/font_noto_sans_common_14_1.bin";
    const std::string font_data = ReadFile(font_file_path);
    Check(!font_data.empty(), "common 文本字体文件必须存在");
    Check(yyjson_get_uint(yyjson_obj_get(text_font, "size_bytes")) == font_data.size(),
          "common 文本字体大小必须与清单一致");
    const char* font_sha = yyjson_get_str(yyjson_obj_get(text_font, "sha256"));
    Check(font_sha != nullptr, "common 文本字体必须记录 SHA-256");
    Sha256 font_hasher;
    font_hasher.Update(font_data);
    Check(ToHex(font_hasher.Final()) == font_sha, "common 文本字体 SHA-256 必须与清单一致");
    Check(yyjson_get_uint(yyjson_obj_get(budget, "common_text_font_bytes")) == font_data.size(),
          "budget.common_text_font_bytes 必须与字体实际大小一致");
    Check(yyjson_get_uint(yyjson_obj_get(budget, "total_bytes")) == total_bytes + font_data.size(),
          "budget.total_bytes 必须等于 GIF 与 common 字体总字节数");

    // 受控标识集合必须与 SparkBotPresentationAdapter 的 allowlist 完全一致，
    // 防止 manifest 与 Adapter 校验失同步。
    {
        const std::string_view kExpectedIds[] = {
            "boot",      "connecting",   "error",  "happy",    "idle",
            "listening", "provisioning", "sleepy", "speaking", "thinking",
        };
        std::set<std::string> expected_ids(kExpectedIds, kExpectedIds + 10);
        Check(expected_ids == asset_ids, "manifest asset_id 集合必须与受控 allowlist 一致");
        for (const std::string& id : asset_ids) {
            Check(voicelife::display_sparkbot::IsControlledAssetId(id), "每个清单 asset_id 必须能被受控解析器接受");
        }
    }

    yyjson_doc_free(doc);
    return 0;
}
