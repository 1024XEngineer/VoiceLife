#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

#include "voicelife/contracts/status.h"

namespace voicelife::storage_fatfs {

/**
 * @brief FATFS/Wear Levelling 数据卷的构造配置。
 *
 * 该配置只描述产品需要的存储参数，不暴露 ESP-IDF 的句柄或配置结构。
 * 生产适配器始终将挂载失败后的自动格式化关闭；配置中因此没有“允许格式化”开关。
 */
struct FatFsVolumeConfig {
    /** @brief 分区表中的数据分区标签。 */
    std::string partition_label = "voicelife";
    /** @brief 通过 VFS 暴露的 FATFS 路径前缀。 */
    std::string base_path = "/data";
    /** @brief FATFS 允许同时打开的文件数量。 */
    int max_files = 6;
    /** @brief 显式格式化时使用的分配单元大小；零表示由 FATFS 选择。 */
    std::size_t allocation_unit_size = 4096;
    /** @brief 是否启用 FATFS 磁盘状态检查；Flash WL 卷通常保持关闭。 */
    bool disk_status_check_enable = false;
    /** @brief 显式格式化时是否使用单 FAT 表。 */
    bool use_one_fat = false;
    /**
     * @brief 期望的数据分区起始地址，单位为字节。
     *
     * 为零时不校验；非零时挂载前必须与分区表中的地址完全一致。该字段由板级
     * Storage Profile 提供，适配器不内置某一块板的布局。
     */
    std::uint32_t expected_partition_address = 0;
    /**
     * @brief 期望的数据分区容量，单位为字节。
     *
     * 为零时不校验；非零时挂载前必须与分区表中的容量完全一致。
     */
    std::uint32_t expected_partition_size = 0;
};

/**
 * @brief 返回当前构建配置对应的默认数据卷配置。
 * @return 根据 ESP-IDF Kconfig 或主机默认值构造的配置。
 */
[[nodiscard]] FatFsVolumeConfig DefaultFatFsVolumeConfig();

/**
 * @brief FATFS 数据卷的容量信息。
 *
 * total_bytes 和 free_bytes 都是 FATFS 报告的逻辑容量，不代表未经 Wear Levelling
 * 折算的物理 Flash 容量。
 */
struct FatFsVolumeCapacity {
    /** @brief FATFS 逻辑总容量，单位为字节。 */
    std::uint64_t total_bytes = 0;
    /** @brief FATFS 当前可用容量，单位为字节。 */
    std::uint64_t free_bytes = 0;
};

/**
 * @brief 管理一个 FATFS/Wear Levelling 数据卷的挂载生命周期。
 *
 * 一个实例只负责一个 VFS 路径和一个 Flash 分区。对象创建不会触碰硬件，必须显式调用
 * Mount() 才会注册 VFS、初始化 Wear Levelling 并挂载 FATFS。析构时会尽力卸载卷；正式
 * 关闭路径应显式调用 Unmount()，以便处理并记录返回状态。
 */
class FatFsVolume final {
   public:
    /**
     * @brief 创建数据卷生命周期管理器。
     * @param config 分区、挂载路径和 FATFS 参数。
     */
    explicit FatFsVolume(FatFsVolumeConfig config = DefaultFatFsVolumeConfig());

    /** @brief 在已挂载时尽力卸载卷并释放资源。 */
    ~FatFsVolume();

    /** @brief 数据卷不能复制。 */
    FatFsVolume(const FatFsVolume&) = delete;
    /** @brief 数据卷不能复制赋值。 */
    FatFsVolume& operator=(const FatFsVolume&) = delete;
    /** @brief 数据卷不能移动，以保持内部生命周期地址稳定。 */
    FatFsVolume(FatFsVolume&&) = delete;
    /** @brief 数据卷不能移动赋值。 */
    FatFsVolume& operator=(FatFsVolume&&) = delete;

    /**
     * @brief 校验配置但不执行挂载。
     * @return 配置合法时返回成功状态，否则返回参数错误。
     */
    [[nodiscard]] Status Validate() const;

    /**
     * @brief 挂载 FATFS/Wear Levelling 数据卷。
     *
     * 重复调用是幂等的；正式路径固定使用 format_if_mount_failed=false，不会因卷损坏
     * 或分区未初始化而清除已有数据。
     * @return 挂载成功时返回成功状态，否则返回已映射的项目状态。
     */
    Status Mount();

    /**
     * @brief 卸载 FATFS/Wear Levelling 数据卷。
     *
     * 未挂载时调用是幂等的。调用后即使底层返回错误，也会将对象标记为未挂载，避免
     * 上层继续使用可能已经部分释放的底层句柄。
     * @return 卸载成功时返回成功状态，否则返回已映射的项目状态。
     */
    Status Unmount();

    /**
     * @brief 查询卷是否处于已挂载状态。
     * @return 已成功挂载且尚未执行卸载时返回 true。
     */
    [[nodiscard]] bool IsMounted() const;

    /**
     * @brief 查询 FATFS 报告的逻辑容量。
     * @return 容量查询结果；未挂载或底层查询失败时返回失败状态。
     */
    [[nodiscard]] Result<FatFsVolumeCapacity> Capacity() const;

    /**
     * @brief 返回不可变的卷配置。
     * @return 构造时保存的配置引用。
     */
    [[nodiscard]] const FatFsVolumeConfig& config() const { return config_; }

   private:
    /**
     * @brief 将底层错误转换为项目状态。
     * @param result ESP-IDF 返回的错误值；主机实现不会调用此方法。
     * @param operation 失败操作的中文说明。
     * @return 映射后的项目状态。
     */
    [[nodiscard]] Status MapPlatformError(int result, const char* operation) const;

    FatFsVolumeConfig config_;
    mutable std::mutex mutex_;
    bool mounted_ = false;
    // wl_handle_t 在 ESP-IDF 中是 int32_t；使用基础类型保持公开头与 IDF 解耦。
    std::int32_t wear_levelling_handle_ = -1;
};

}  // namespace voicelife::storage_fatfs
