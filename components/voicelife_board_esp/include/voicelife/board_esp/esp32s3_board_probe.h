#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "voicelife/board_esp/sparkbot_profile.h"

namespace voicelife::board_esp {

/** @brief 只读 Flash 分区表项。 */
struct BoardPartitionRecord {
    /** @brief 分区标签。 */
    std::string label;
    /** @brief 分区类型。 */
    uint8_t type = 0;
    /** @brief 分区子类型。 */
    uint8_t subtype = 0;
    /** @brief 分区起始地址。 */
    uint32_t address = 0;
    /** @brief 分区大小，单位字节。 */
    uint32_t size = 0;
    /** @brief 分区是否启用加密。 */
    bool encrypted = false;
    /** @brief 分区是否只读。 */
    bool readonly = false;
};

/** @brief ESP32-S3 身份与分区只读探针报告。 */
struct BoardProbeReport {
    /** @brief 芯片型号文本。 */
    std::string chip_model;
    /** @brief 芯片 revision。 */
    int chip_revision = -1;
    /** @brief 芯片核心数。 */
    int chip_cores = 0;
    /** @brief Flash 容量，单位字节。 */
    uint32_t flash_bytes = 0;
    /** @brief PSRAM 容量，单位字节。 */
    uint32_t psram_bytes = 0;
    /** @brief MAC 的不可逆短指纹，不输出原始 MAC。 */
    std::string mac_fingerprint;
    /** @brief Profile 提供的 SKU，硬件本身没有通用 SKU 查询接口。 */
    std::string expected_sku;
    /** @brief 是否完成了硬件 SKU 独立确认。 */
    bool sku_verified = false;
    /** @brief 只读分区表快照。 */
    std::vector<BoardPartitionRecord> partitions;

    /**
     * @brief 判断报告是否达到 Profile 的芯片、容量和分区观测要求。
     * @param profile 要比对的 SparkBot Profile。
     * @return 匹配时返回 true。
     */
    [[nodiscard]] bool matches_profile(const SparkBotBoardProfile& profile) const;
};

/**
 * @brief 执行 ESP32-S3 身份与分区只读探针。
 *
 * 主机测试不会伪造设备探针；非 ESP 平台调用 Run 会返回 kUnavailable。
 */
class Esp32s3BoardProbe final {
   public:
    /** @brief 构造探针。 */
    Esp32s3BoardProbe();
    /** @brief 析构探针。 */
    ~Esp32s3BoardProbe();

    /** @brief 禁止拷贝构造。 */
    Esp32s3BoardProbe(const Esp32s3BoardProbe&) = delete;
    /** @brief 禁止拷贝赋值。 */
    Esp32s3BoardProbe& operator=(const Esp32s3BoardProbe&) = delete;

    /**
     * @brief 读取当前芯片、容量、MAC 指纹和分区表。
     * @param profile 提供预期目标和 SKU 的板级 Profile。
     * @return 只读探针报告。
     */
    [[nodiscard]] Result<BoardProbeReport> Run(const SparkBotBoardProfile& profile) const;

   private:
    /** @brief 保留 ABI 的实现占位。 */
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace voicelife::board_esp
