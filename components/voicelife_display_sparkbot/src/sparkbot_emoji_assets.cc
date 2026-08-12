#include "voicelife/display_sparkbot/sparkbot_emoji_assets.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <string_view>

#ifdef ESP_PLATFORM
#include <esp_log.h>
#include <esp_partition.h>
#include <spi_flash_mmap.h>
#endif

namespace voicelife::display_sparkbot {

namespace {

/** @brief 官方 SparkBot 牛头表情的受控标识列表（与 manifest.json 一致）。 */
constexpr std::array<std::string_view, 10> kControlledAssetIds = {
    "boot", "connecting", "error", "happy", "idle", "listening", "provisioning", "sleepy", "speaking", "thinking",
};

#ifdef ESP_PLATFORM
constexpr const char* kTag = "sparkbot_emoji";
constexpr const char* kPartitionLabel = "assets";

/** @brief 官方 assets 分区文件表项（xiaozhi-esp32@37d1aee main/assets.cc）。 */
struct MmappedAssetEntry {
    char name[32];
    uint32_t size;
    uint32_t offset;
    uint16_t width;
    uint16_t height;
};

/** @brief 官方 assets 分区头部：文件数 + 校验和 + 数据长度。 */
constexpr std::size_t kHeaderBytes = 12;

uint16_t CalculateChecksum(const uint8_t* data, uint32_t length) {
    uint32_t checksum = 0;
    for (uint32_t i = 0; i < length; ++i) {
        checksum += data[i];
    }
    return static_cast<uint16_t>(checksum & 0xFFFF);
}
#endif

}  // namespace

bool IsControlledAssetId(std::string_view asset_id) {
    if (asset_id.empty() || asset_id.find('/') != std::string_view::npos ||
        asset_id.find('\\') != std::string_view::npos || asset_id.find("..") != std::string_view::npos) {
        return false;
    }
    return std::find(kControlledAssetIds.begin(), kControlledAssetIds.end(), asset_id) != kControlledAssetIds.end();
}

std::string_view AssetFilenameForId(std::string_view asset_id) {
    if (!IsControlledAssetId(asset_id)) {
        return {};
    }
    // manifest 的 file 字段为固定的单段文件名；不由 Runtime 或调用方传入。
    static constexpr std::array<std::pair<std::string_view, std::string_view>, 10> kAssetFiles = {
        std::pair{"boot", "boot.gif"},
        std::pair{"connecting", "connecting.gif"},
        std::pair{"error", "error.gif"},
        std::pair{"happy", "happy.gif"},
        std::pair{"idle", "idle.gif"},
        std::pair{"listening", "listening.gif"},
        std::pair{"provisioning", "provisioning.gif"},
        std::pair{"sleepy", "sleepy.gif"},
        std::pair{"speaking", "speaking.gif"},
        std::pair{"thinking", "thinking.gif"},
    };
    for (const auto& [id, filename] : kAssetFiles) {
        if (id == asset_id) {
            return filename;
        }
    }
    return {};
}

SparkBotEmojiAssets::~SparkBotEmojiAssets() {
#ifdef ESP_PLATFORM
    if (mmap_handle_ != nullptr) {
        esp_partition_munmap(static_cast<spi_flash_mmap_handle_t>(reinterpret_cast<uintptr_t>(mmap_handle_)));
        mmap_handle_ = nullptr;
        mmap_root_ = nullptr;
    }
#endif
}

voicelife::Status SparkBotEmojiAssets::Initialize() {
#ifdef ESP_PLATFORM
    const esp_partition_t* partition =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, kPartitionLabel);
    if (partition == nullptr) {
        return voicelife::Status::Error(voicelife::ErrorCode::kNotFound, "未找到 assets 分区");
    }
    const void* mmap_root = nullptr;
    spi_flash_mmap_handle_t mmap_handle = 0;
    const esp_err_t err =
        esp_partition_mmap(partition, 0, partition->size, ESP_PARTITION_MMAP_DATA, &mmap_root, &mmap_handle);
    if (err != ESP_OK) {
        return voicelife::Status::Error(voicelife::ErrorCode::kInternal, "assets 分区 mmap 失败");
    }

    const auto* root = static_cast<const uint8_t*>(mmap_root);
    const uint32_t stored_files = *reinterpret_cast<const uint32_t*>(root + 0);
    const uint32_t stored_checksum = *reinterpret_cast<const uint32_t*>(root + 4);
    const uint32_t stored_len = *reinterpret_cast<const uint32_t*>(root + 8);
    if (stored_len > partition->size - kHeaderBytes) {
        esp_partition_munmap(mmap_handle);
        return voicelife::Status::Error(voicelife::ErrorCode::kInternal, "assets 分区数据长度非法");
    }
    // 文件表边界：表字节数必须落在 stored_len 内，文件数不超过受控集合数量。
    constexpr std::size_t kMaxControlledFiles = 10;
    const std::size_t table_bytes = static_cast<std::size_t>(stored_files) * sizeof(MmappedAssetEntry);
    if (stored_files > kMaxControlledFiles || table_bytes > stored_len) {
        esp_partition_munmap(mmap_handle);
        return voicelife::Status::Error(voicelife::ErrorCode::kInternal, "assets 分区文件表越界");
    }
    // 逐项校验：偏移 + ZZ(2) + 大小 不溢出数据区。
    const auto* table = reinterpret_cast<const MmappedAssetEntry*>(root + kHeaderBytes);
    for (uint32_t i = 0; i < stored_files; ++i) {
        const std::size_t entry_offset = kHeaderBytes + sizeof(MmappedAssetEntry) * stored_files + table[i].offset;
        if (entry_offset + 2 + table[i].size > kHeaderBytes + stored_len || entry_offset + 2 < kHeaderBytes) {
            esp_partition_munmap(mmap_handle);
            return voicelife::Status::Error(voicelife::ErrorCode::kInternal, "assets 分区表项越界");
        }
    }
    const uint16_t calculated_checksum = CalculateChecksum(root + kHeaderBytes, stored_len);
    if (calculated_checksum != static_cast<uint16_t>(stored_checksum)) {
        esp_partition_munmap(mmap_handle);
        return voicelife::Status::Error(voicelife::ErrorCode::kInternal, "assets 分区校验和错误");
    }

    mmap_root_ = mmap_root;
    mmap_handle_ = reinterpret_cast<void*>(static_cast<uintptr_t>(mmap_handle));
    initialized_ = true;
    ESP_LOGI(kTag, "SPARKBOT_ASSETS_MMAP_OK=1 files=%u", static_cast<unsigned>(stored_files));
    return voicelife::Status::Ok();
#else
    (void)0;
    return voicelife::Status::Error(voicelife::ErrorCode::kUnavailable, "主机构建不 mmap assets 分区");
#endif
}

voicelife::Result<GifAssetView> SparkBotEmojiAssets::Load(std::string_view asset_id) {
    if (!IsControlledAssetId(asset_id)) {
        return voicelife::Result<GifAssetView>::Failure(voicelife::ErrorCode::kInvalidArgument,
                                                        "资源标识必须是非空、无路径分隔符的受控名称");
    }
#ifdef ESP_PLATFORM
    if (!initialized_) {
        return voicelife::Result<GifAssetView>::Failure(voicelife::ErrorCode::kUnavailable, "assets 分区尚未初始化");
    }
    const auto* root = static_cast<const uint8_t*>(mmap_root_);
    const uint32_t stored_files = *reinterpret_cast<const uint32_t*>(root + 0);
    const auto* table = reinterpret_cast<const MmappedAssetEntry*>(root + kHeaderBytes);
    const std::string_view filename = AssetFilenameForId(asset_id);
    for (uint32_t i = 0; i < stored_files; ++i) {
        const MmappedAssetEntry& item = table[i];
        if (std::string_view(item.name, strnlen(item.name, sizeof(item.name))) != filename) {
            continue;
        }
        // 边界校验：表 + 偏移 + ZZ(2) + 资源大小必须落在分区数据区内。
        const uint32_t stored_len = *reinterpret_cast<const uint32_t*>(root + 8);
        const std::size_t data_region = kHeaderBytes + stored_len;
        const std::size_t offset = kHeaderBytes + sizeof(MmappedAssetEntry) * stored_files + item.offset;
        if (offset + 2 + item.size > data_region || offset + 2 < kHeaderBytes) {
            return voicelife::Result<GifAssetView>::Failure(voicelife::ErrorCode::kInternal,
                                                            "资源超出 assets 分区边界");
        }
        const auto* data = static_cast<const char*>(mmap_root_) + offset;
        if (data[0] != 'Z' || data[1] != 'Z') {
            return voicelife::Result<GifAssetView>::Failure(voicelife::ErrorCode::kInternal, "资源缺少 ZZ magic");
        }
        ESP_LOGI(kTag, "SPARKBOT_GIF_LOADED asset=%.*s", static_cast<int>(filename.size()), filename.data());
        return voicelife::Result<GifAssetView>::Success(GifAssetView{.data = data + 2, .size = item.size});
    }
    ESP_LOGW(kTag, "SPARKBOT_GIF_LOAD_FAILED asset=%.*s", static_cast<int>(filename.size()), filename.data());
    return voicelife::Result<GifAssetView>::Failure(voicelife::ErrorCode::kNotFound, "资源不在 assets 分区中");
#else
    (void)asset_id;
    return voicelife::Result<GifAssetView>::Failure(voicelife::ErrorCode::kUnavailable,
                                                    "主机构建不加载 assets 分区资源");
#endif
}

}  // namespace voicelife::display_sparkbot
