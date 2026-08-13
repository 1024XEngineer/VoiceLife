#include "voicelife/board_esp/sparkbot_wake_model_assets.h"

#include <cstring>
#include <string_view>

#ifdef ESP_PLATFORM
#include <esp_log.h>
#include <esp_partition.h>
#include <spi_flash_mmap.h>
#endif

namespace voicelife::board_esp {
namespace {

#ifdef ESP_PLATFORM
constexpr char kTag[] = "sparkbot_wake_assets";
constexpr char kPartitionLabel[] = "assets";
constexpr char kModelFilename[] = "srmodels.bin";
constexpr std::size_t kHeaderBytes = 12;
constexpr std::size_t kEntryBytes = 44;
constexpr std::size_t kMaxFiles = 12;

struct MmappedAssetEntry {
    char name[32];
    uint32_t size;
    uint32_t offset;
    uint16_t width;
    uint16_t height;
};

uint16_t Checksum(const uint8_t* data, uint32_t size) {
    uint32_t result = 0;
    for (uint32_t i = 0; i < size; ++i) result += data[i];
    return static_cast<uint16_t>(result & 0xffffU);
}
#endif

}  // namespace

SparkBotWakeModelAssets::~SparkBotWakeModelAssets() {
#ifdef ESP_PLATFORM
    if (mmap_handle_ != nullptr) {
        esp_partition_munmap(static_cast<spi_flash_mmap_handle_t>(reinterpret_cast<uintptr_t>(mmap_handle_)));
    }
#endif
}

Status SparkBotWakeModelAssets::Initialize() {
#ifdef ESP_PLATFORM
    if (model_root_ != nullptr) return Status::Ok();
    const esp_partition_t* partition =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, kPartitionLabel);
    if (partition == nullptr) return Status::Error(ErrorCode::kNotFound, "未找到 SparkBot assets 分区");

    const void* root_void = nullptr;
    spi_flash_mmap_handle_t handle = 0;
    if (esp_partition_mmap(partition, 0, partition->size, ESP_PARTITION_MMAP_DATA, &root_void, &handle) != ESP_OK) {
        return Status::Error(ErrorCode::kUnavailable, "WakeNet assets mmap 失败");
    }
    const auto* root = static_cast<const uint8_t*>(root_void);
    const uint32_t file_count = *reinterpret_cast<const uint32_t*>(root);
    const uint32_t stored_checksum = *reinterpret_cast<const uint32_t*>(root + 4);
    const uint32_t stored_length = *reinterpret_cast<const uint32_t*>(root + 8);
    const std::size_t table_size = static_cast<std::size_t>(file_count) * kEntryBytes;
    if (file_count == 0 || file_count > kMaxFiles || stored_length > partition->size - kHeaderBytes ||
        table_size > stored_length || Checksum(root + kHeaderBytes, stored_length) != static_cast<uint16_t>(stored_checksum)) {
        esp_partition_munmap(handle);
        return Status::Error(ErrorCode::kInternal, "WakeNet assets 清单校验失败");
    }
    const auto* table = reinterpret_cast<const MmappedAssetEntry*>(root + kHeaderBytes);
    for (uint32_t index = 0; index < file_count; ++index) {
        const auto& entry = table[index];
        if (std::string_view(entry.name, strnlen(entry.name, sizeof(entry.name))) != kModelFilename) continue;
        const std::size_t offset = kHeaderBytes + table_size + entry.offset;
        if (entry.width != 0 || entry.height != 0 || entry.size < 4 || offset + 2 + entry.size > kHeaderBytes + stored_length ||
            root[offset] != 'Z' || root[offset + 1] != 'Z') {
            esp_partition_munmap(handle);
            return Status::Error(ErrorCode::kInternal, "WakeNet srmodels.bin 记录非法");
        }
        mmap_root_ = root_void;
        mmap_handle_ = reinterpret_cast<void*>(static_cast<uintptr_t>(handle));
        model_root_ = root + offset + 2;
        model_size_ = entry.size;
        ESP_LOGI(kTag, "WAKE_MODEL_ASSET_READY bytes=%u", static_cast<unsigned>(entry.size));
        return Status::Ok();
    }
    esp_partition_munmap(handle);
    return Status::Error(ErrorCode::kNotFound, "assets 分区缺少受控 srmodels.bin");
#else
    return Status::Error(ErrorCode::kUnavailable, "WakeNet assets 仅支持 ESP 平台");
#endif
}

}  // namespace voicelife::board_esp
