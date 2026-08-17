#include "voicelife/board_esp/esp32s3_board_probe.h"

#include <array>
#include <cstdint>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <utility>

#ifdef ESP_PLATFORM

#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_mac.h"
#include "esp_partition.h"
#include "esp_psram.h"
#include "esp_system.h"

#endif

namespace voicelife::board_esp {
namespace {

#ifndef ESP_PLATFORM
Result<BoardProbeReport> HostUnavailable() {
    return Result<BoardProbeReport>::Failure(ErrorCode::kUnavailable, "ESP32-S3 板级探针只能在 ESP-IDF 目标上运行");
}
#endif

}  // namespace

bool BoardProbeReport::matches_profile(const SparkBotBoardProfile& profile) const {
    return profile.Validate().ok() && chip_model == "ESP32-S3" && flash_bytes == profile.expected_flash_bytes &&
           psram_bytes == profile.expected_psram_bytes && !partitions.empty();
}

class Esp32s3BoardProbe::Impl {};

Esp32s3BoardProbe::Esp32s3BoardProbe() : impl_(std::make_unique<Impl>()) {}

Esp32s3BoardProbe::~Esp32s3BoardProbe() = default;

Result<BoardProbeReport> Esp32s3BoardProbe::Run(const SparkBotBoardProfile& profile) const {
    const Status profile_status = profile.Validate();
    if (!profile_status.ok()) {
        return Result<BoardProbeReport>::Failure(profile_status.code, profile_status.message);
    }

#ifdef ESP_PLATFORM
    esp_chip_info_t chip_info{};
    esp_chip_info(&chip_info);
    if (chip_info.model != CHIP_ESP32S3) {
        return Result<BoardProbeReport>::Failure(ErrorCode::kUnavailable, "当前芯片不是 ESP32-S3");
    }

    uint32_t flash_bytes = 0;
    if (esp_flash_get_size(esp_flash_default_chip, &flash_bytes) != ESP_OK) {
        return Result<BoardProbeReport>::Failure(ErrorCode::kUnavailable, "无法读取 Flash 容量");
    }

    uint8_t mac[6]{};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
        return Result<BoardProbeReport>::Failure(ErrorCode::kUnavailable, "无法读取设备 MAC");
    }

    uint64_t fingerprint = 1469598103934665603ULL;
    for (const uint8_t byte : mac) {
        fingerprint ^= byte;
        fingerprint *= 1099511628211ULL;
    }
    std::ostringstream fingerprint_stream;
    fingerprint_stream << std::hex << std::setw(16) << std::setfill('0') << fingerprint;

    BoardProbeReport report;
    report.chip_model = "ESP32-S3";
    report.chip_revision = chip_info.revision;
    report.chip_cores = chip_info.cores;
    report.flash_bytes = flash_bytes;
    report.psram_bytes = static_cast<uint32_t>(esp_psram_get_size());
    report.mac_fingerprint = fingerprint_stream.str();
    report.expected_sku = profile.sku;
    report.sku_verified = false;

    esp_partition_iterator_t iterator = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, nullptr);
    while (iterator != nullptr) {
        const esp_partition_t* partition = esp_partition_get(iterator);
        if (partition != nullptr) {
            report.partitions.push_back({.label = partition->label,
                                         .type = partition->type,
                                         .subtype = partition->subtype,
                                         .address = partition->address,
                                         .size = partition->size,
                                         .encrypted = partition->encrypted,
                                         .readonly = partition->readonly});
        }
        iterator = esp_partition_next(iterator);
    }
    esp_partition_iterator_release(iterator);
    return Result<BoardProbeReport>::Success(std::move(report));
#else
    (void)profile;
    return HostUnavailable();
#endif
}

}  // namespace voicelife::board_esp
