#include "voicelife/storage_fatfs/fatfs_volume.h"

#include <algorithm>
#include <cstdint>
#include <utility>

#ifdef ESP_PLATFORM
#include "esp_err.h"
#include "esp_partition.h"
#include "esp_vfs_fat.h"
#include "sdkconfig.h"
#include "wear_levelling.h"
#endif

namespace voicelife::storage_fatfs {
namespace {

/**
 * @brief 创建配置错误状态。
 * @param message 配置错误说明。
 * @return 带有 invalid_argument 分类的状态。
 */
Status InvalidConfig(std::string message) { return Status::Error(ErrorCode::kInvalidArgument, std::move(message)); }

/**
 * @brief 判断整数是否为 2 的幂。
 * @param value 待判断的整数。
 * @return value 非零且为 2 的幂时返回 true。
 */
bool IsPowerOfTwo(std::size_t value) { return value != 0 && (value & (value - 1U)) == 0; }

/**
 * @brief 判断文本是否含有 C 字符串不能安全承载的 NUL 字节。
 * @param value 待检查的文本。
 * @return 含有 NUL 字节时返回 true。
 */
bool ContainsNul(const std::string& value) { return std::find(value.begin(), value.end(), '\0') != value.end(); }

}  // namespace

FatFsVolumeConfig DefaultFatFsVolumeConfig() {
    FatFsVolumeConfig config;
#ifdef ESP_PLATFORM
    config.partition_label = CONFIG_VOICELIFE_STORAGE_FATFS_PARTITION_LABEL;
    config.base_path = CONFIG_VOICELIFE_STORAGE_FATFS_BASE_PATH;
    config.max_files = CONFIG_VOICELIFE_STORAGE_FATFS_MAX_FILES;
    config.allocation_unit_size = CONFIG_VOICELIFE_STORAGE_FATFS_ALLOCATION_UNIT_SIZE;
    config.expected_partition_address = CONFIG_VOICELIFE_STORAGE_FATFS_EXPECTED_PARTITION_ADDRESS;
    config.expected_partition_size = CONFIG_VOICELIFE_STORAGE_FATFS_EXPECTED_PARTITION_SIZE;
#endif
    return config;
}

FatFsVolume::FatFsVolume(FatFsVolumeConfig config) : config_(std::move(config)) {}

FatFsVolume::~FatFsVolume() { (void)Unmount(); }

Status FatFsVolume::Validate() const {
    if (config_.partition_label.empty() || config_.partition_label.size() > 15) {
        return InvalidConfig("FATFS 分区标签必须为 1 到 15 个字符");
    }
    if (ContainsNul(config_.partition_label)) {
        return InvalidConfig("FATFS 分区标签不能包含 NUL 字节");
    }
    if (config_.base_path.size() < 2 || config_.base_path.size() > 15 || config_.base_path.front() != '/' ||
        config_.base_path.back() == '/') {
        return InvalidConfig("FATFS 挂载路径必须以 / 开头、不能以 / 结尾且长度不超过 15");
    }
    if (ContainsNul(config_.base_path)) {
        return InvalidConfig("FATFS 挂载路径不能包含 NUL 字节");
    }
    if (config_.max_files <= 0 || config_.max_files > 32) {
        return InvalidConfig("FATFS 最大打开文件数必须在 1 到 32 之间");
    }
    if (config_.allocation_unit_size != 0 &&
        (!IsPowerOfTwo(config_.allocation_unit_size) || config_.allocation_unit_size < 512 ||
         config_.allocation_unit_size > 128U * 4096U)) {
        return InvalidConfig("FATFS 分配单元大小必须为 0 或 512 到 524288 之间的 2 次幂");
    }
    return Status::Ok();
}

#ifdef ESP_PLATFORM

Status FatFsVolume::MapPlatformError(int result, const char* operation) const {
    if (result == ESP_OK) return Status::Ok();

    ErrorCode code = ErrorCode::kInternal;
    switch (result) {
        case ESP_ERR_INVALID_ARG:
        case ESP_ERR_INVALID_SIZE:
            code = ErrorCode::kInvalidArgument;
            break;
        case ESP_ERR_NOT_FOUND:
            code = ErrorCode::kNotFound;
            break;
        case ESP_ERR_INVALID_STATE:
            code = ErrorCode::kConflict;
            break;
        case ESP_ERR_NO_MEM:
        case ESP_ERR_TIMEOUT:
        case ESP_ERR_NOT_SUPPORTED:
        case ESP_ERR_NOT_ALLOWED:
            code = ErrorCode::kUnavailable;
            break;
        default:
            code = ErrorCode::kUnavailable;
            break;
    }

    const char* detail = esp_err_to_name(static_cast<esp_err_t>(result));
    std::string message = operation == nullptr ? "FATFS 操作失败" : operation;
    message += "：";
    message += detail == nullptr ? "未知 ESP-IDF 错误" : detail;
    message += "(" + std::to_string(result) + ")";
    return Status::Error(code, std::move(message));
}

Status FatFsVolume::Mount() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (mounted_) return Status::Ok();

    const Status validation = Validate();
    if (!validation.ok()) return validation;

    // 先按标签读取分区表，确保板级 Storage Profile 不会误挂载同名但位置变化的分区。
    const esp_partition_t* partition =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, config_.partition_label.c_str());
    if (partition == nullptr) {
        return MapPlatformError(ESP_ERR_NOT_FOUND, "找不到 FATFS 数据分区");
    }
    if (config_.expected_partition_address != 0 && partition->address != config_.expected_partition_address) {
        return Status::Error(ErrorCode::kConflict, "FATFS 数据分区起始地址与 Storage Profile 不一致");
    }
    if (config_.expected_partition_size != 0 && partition->size != config_.expected_partition_size) {
        return Status::Error(ErrorCode::kConflict, "FATFS 数据分区容量与 Storage Profile 不一致");
    }

    // 生产数据卷禁止挂载失败时自动格式化；卷损坏必须显式报错，避免静默丢失日程与提醒记录。
    esp_vfs_fat_mount_config_t mount_config = VFS_FAT_MOUNT_DEFAULT_CONFIG();
    mount_config.format_if_mount_failed = false;
    mount_config.max_files = config_.max_files;
    mount_config.allocation_unit_size = config_.allocation_unit_size;
    mount_config.disk_status_check_enable = config_.disk_status_check_enable;
    mount_config.use_one_fat = config_.use_one_fat;

    wl_handle_t handle = WL_INVALID_HANDLE;
    const esp_err_t result = esp_vfs_fat_spiflash_mount_rw_wl(config_.base_path.c_str(),
                                                              config_.partition_label.c_str(), &mount_config, &handle);
    if (result != ESP_OK) {
        wear_levelling_handle_ = -1;
        mounted_ = false;
        return MapPlatformError(result, "挂载 FATFS/Wear Levelling 数据卷失败");
    }

    wear_levelling_handle_ = static_cast<std::int32_t>(handle);
    mounted_ = true;
    return Status::Ok();
}

Status FatFsVolume::Unmount() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!mounted_) return Status::Ok();

    const wl_handle_t handle = static_cast<wl_handle_t>(wear_levelling_handle_);
    const esp_err_t result = esp_vfs_fat_spiflash_unmount_rw_wl(config_.base_path.c_str(), handle);
    mounted_ = false;
    wear_levelling_handle_ = -1;
    return MapPlatformError(result, "卸载 FATFS/Wear Levelling 数据卷失败");
}

bool FatFsVolume::IsMounted() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return mounted_;
}

Result<FatFsVolumeCapacity> FatFsVolume::Capacity() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!mounted_) {
        return Result<FatFsVolumeCapacity>::Failure(ErrorCode::kUnavailable, "FATFS 数据卷尚未挂载");
    }

    std::uint64_t total_bytes = 0;
    std::uint64_t free_bytes = 0;
    const esp_err_t result = esp_vfs_fat_info(config_.base_path.c_str(), &total_bytes, &free_bytes);
    if (result != ESP_OK) {
        const Status status = MapPlatformError(result, "查询 FATFS 数据卷容量失败");
        return Result<FatFsVolumeCapacity>::Failure(status.code, status.message);
    }
    return Result<FatFsVolumeCapacity>::Success({total_bytes, free_bytes});
}

#else

Status FatFsVolume::MapPlatformError(int, const char* operation) const {
    return Status::Error(ErrorCode::kUnavailable, operation == nullptr ? "FATFS 仅支持 ESP-IDF 目标" : operation);
}

Status FatFsVolume::Mount() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (mounted_) return Status::Ok();
    const Status validation = Validate();
    if (!validation.ok()) return validation;
    return MapPlatformError(0, "主机环境不提供 FATFS/Wear Levelling 硬件挂载");
}

Status FatFsVolume::Unmount() {
    std::lock_guard<std::mutex> lock(mutex_);
    mounted_ = false;
    wear_levelling_handle_ = -1;
    return Status::Ok();
}

bool FatFsVolume::IsMounted() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return mounted_;
}

Result<FatFsVolumeCapacity> FatFsVolume::Capacity() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return Result<FatFsVolumeCapacity>::Failure(ErrorCode::kUnavailable,
                                                "主机环境不提供 FATFS/Wear Levelling 容量查询");
}

#endif

}  // namespace voicelife::storage_fatfs
