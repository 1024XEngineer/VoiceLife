#include "voicelife/display_sparkbot/sparkbot_emoji_assets.h"

#include "support/test_support.h"

using voicelife::ErrorCode;
using voicelife::test::Check;

int main() {
    using voicelife::display_sparkbot::AssetFilenameForId;
    using voicelife::display_sparkbot::IsControlledAssetId;
    using voicelife::display_sparkbot::SparkBotEmojiAssets;

    // 受控标识：官方 key 和待机屏保 key 全部接受。
    const std::string_view kAllIds[] = {
        "boot",      "connecting",   "error",  "happy",    "idle",     "idle_eyes",
        "listening", "provisioning", "sleepy", "speaking", "thinking", "dizzy",
    };
    for (const std::string_view id : kAllIds) {
        Check(IsControlledAssetId(id), "官方 asset_id 必须被接受");
        const std::string_view filename = AssetFilenameForId(id);
        Check(filename.size() == id.size() + 4 && filename.substr(0, id.size()) == id &&
                  filename.substr(id.size()) == ".gif",
              "受控逻辑 asset_id 必须在 SparkBot adapter 内映射为固定 GIF 文件名");
    }

    // 非法格式与未知资源必须被拒绝（不接受 URL/任意路径/任意字节流）。
    Check(!IsControlledAssetId(""), "空 asset_id 必须拒绝");
    Check(!IsControlledAssetId("../boot.gif"), "路径特征 asset_id 必须拒绝");
    Check(!IsControlledAssetId("https://example.com/boot.gif"), "URL 形式必须拒绝");
    Check(!IsControlledAssetId("a\\b.gif"), "反斜杠路径必须拒绝");
    Check(!IsControlledAssetId("not_in_manifest"), "清单外资源必须拒绝");
    Check(AssetFilenameForId("not_in_manifest").empty(), "清单外资源不得有文件名映射");

    // host 构建不触碰 assets 分区：Initialize/Load 必须返回明确状态。
    SparkBotEmojiAssets assets;
    Check(assets.Initialize().code == ErrorCode::kUnavailable,
          "host 构建 Initialize 必须返回 kUnavailable（不 mmap 分区）");
    const auto load = assets.Load("boot");
    Check(!load.ok() && load.status.code == ErrorCode::kUnavailable, "host 构建 Load 必须返回 kUnavailable");
    const auto bad = assets.Load("../evil.gif");
    Check(!bad.ok() && bad.status.code == ErrorCode::kInvalidArgument,
          "非法 asset_id 在任何构建下都必须返回 kInvalidArgument");
    const auto font = assets.LoadCommonTextFont();
    Check(!font.ok() && font.status.code == ErrorCode::kUnavailable,
          "host 构建 LoadCommonTextFont 必须返回 kUnavailable");

    return 0;
}
