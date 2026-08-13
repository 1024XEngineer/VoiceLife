#include <string>

#include "support/test_support.h"
#include "voicelife/storage_fatfs/fatfs_volume.h"

using voicelife::ErrorCode;
using voicelife::storage_fatfs::DefaultFatFsVolumeConfig;
using voicelife::storage_fatfs::FatFsVolume;
using voicelife::storage_fatfs::FatFsVolumeConfig;
using voicelife::test::Check;

namespace {

/**
 * @brief 验证默认配置与当前产品存储约定一致。
 * @return 无。
 */
void TestDefaultConfig() {
    const FatFsVolumeConfig config = DefaultFatFsVolumeConfig();
    Check(config.partition_label == "voicelife", "默认分区标签应为 voicelife");
    Check(config.base_path == "/data", "默认挂载路径应为 /data");
    Check(config.max_files == 6, "默认应允许 SQLite 及其辅助文件同时打开");
    Check(config.allocation_unit_size == 4096, "默认分配单元应匹配已验证的 4 KiB Storage Profile");
    Check(!config.disk_status_check_enable && !config.use_one_fat, "默认配置应保留 FATFS 可靠性参数");
}

/**
 * @brief 验证配置校验拒绝会被 ESP-IDF C API 截断或拒绝的字段。
 * @return 无。
 */
void TestConfigValidation() {
    FatFsVolumeConfig config;
    FatFsVolume valid(config);
    Check(valid.Validate().ok(), "默认 FATFS 数据卷配置应合法");

    config.partition_label.clear();
    Check(FatFsVolume(config).Validate().code == ErrorCode::kInvalidArgument, "空分区标签应被拒绝");
    config.partition_label = std::string(16, 'x');
    Check(FatFsVolume(config).Validate().code == ErrorCode::kInvalidArgument, "超过 15 字符的分区标签应被拒绝");
    config.partition_label = std::string("voice\0life", 10);
    Check(FatFsVolume(config).Validate().code == ErrorCode::kInvalidArgument, "分区标签中的 NUL 字节应被拒绝");

    config.partition_label = "voicelife";
    config.base_path = "data";
    Check(FatFsVolume(config).Validate().code == ErrorCode::kInvalidArgument, "不以 / 开头的挂载路径应被拒绝");
    config.base_path = "/data/";
    Check(FatFsVolume(config).Validate().code == ErrorCode::kInvalidArgument, "以 / 结尾的挂载路径应被拒绝");
    config.base_path = std::string("/da\0ta", 6);
    Check(FatFsVolume(config).Validate().code == ErrorCode::kInvalidArgument, "挂载路径中的 NUL 字节应被拒绝");

    config.base_path = "/data";
    config.max_files = 0;
    Check(FatFsVolume(config).Validate().code == ErrorCode::kInvalidArgument, "非正数最大文件数应被拒绝");
    config.max_files = 33;
    Check(FatFsVolume(config).Validate().code == ErrorCode::kInvalidArgument, "超过配置上限的最大文件数应被拒绝");
    config.max_files = 6;
    config.allocation_unit_size = 1000;
    Check(FatFsVolume(config).Validate().code == ErrorCode::kInvalidArgument, "非 2 次幂分配单元应被拒绝");
    config.allocation_unit_size = 0;
    Check(FatFsVolume(config).Validate().ok(), "零分配单元应委托 FATFS 选择默认值");
}

/**
 * @brief 验证主机实现明确报告硬件不可用且保持未挂载状态。
 * @return 无。
 */
void TestHostContract() {
    FatFsVolume volume;
    Check(!volume.IsMounted(), "数据卷构造后不应隐式挂载");
    Check(volume.Capacity().status.code == ErrorCode::kUnavailable, "未挂载数据卷不能查询容量");
    Check(volume.Mount().code == ErrorCode::kUnavailable, "主机环境不能伪装成已挂载 Flash 数据卷");
    Check(!volume.IsMounted(), "主机挂载失败后必须保持未挂载状态");
    Check(volume.Unmount().ok() && volume.Unmount().ok(), "未挂载数据卷的卸载应保持幂等");
}

}  // namespace

/**
 * @brief 执行 FATFS 数据卷主机契约测试。
 * @return 全部断言通过时返回零。
 */
int main() {
    TestDefaultConfig();
    TestConfigValidation();
    TestHostContract();
    return 0;
}
