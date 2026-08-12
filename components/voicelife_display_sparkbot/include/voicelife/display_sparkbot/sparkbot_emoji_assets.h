#pragma once

#include <cstddef>
#include <string_view>

#include "voicelife/contracts/status.h"

namespace voicelife::display_sparkbot {

/**
 * @brief 校验资源标识是否为受控 asset_id。
 *
 * 与 assets 资源清单（manifest.json）的 10 个官方 key 一致；拒绝空值、
 * 路径分隔符与 ..。host 与 ESP 构建均可使用。
 * @param asset_id 调用方提供的资源标识。
 * @return 是受控 asset_id 时返回 true。
 */
[[nodiscard]] bool IsControlledAssetId(std::string_view asset_id);

/** @brief 从 assets 分区加载的 GIF 资源视图（mmap 只读，不持有所有权）。 */
struct GifAssetView {
    /** @brief GIF 数据指针（已跳过 "ZZ" magic）。 */
    const void* data = nullptr;
    /** @brief GIF 数据大小（字节）。 */
    std::size_t size = 0;
};

/**
 * @brief SparkBot emoji GIF 资源加载器（官方 assets 分区格式移植）。
 *
 * 移植来源：xiaozhi-esp32@37d1aee main/assets.cc 的 LvglStrategy
 * （esp_partition_mmap + 12B 头 + 44B/项文件表 + "ZZ" magic）。只接受
 * 受控 asset_id，不接受 URL、任意路径或任意字节流；运行时不做网络下载。
 * host 构建不触碰分区，Initialize/Load 返回 kUnavailable。
 */
class SparkBotEmojiAssets {
   public:
    /** @brief 构造函数。 */
    SparkBotEmojiAssets() = default;
    /** @brief 析构函数：释放 mmap。 */
    ~SparkBotEmojiAssets();

    /** @brief 禁止拷贝构造。 */
    SparkBotEmojiAssets(const SparkBotEmojiAssets&) = delete;
    /** @brief 禁止拷贝赋值。 */
    SparkBotEmojiAssets& operator=(const SparkBotEmojiAssets&) = delete;

    /**
     * @brief mmap assets 分区并校验头部与校验和。
     * @return 初始化结果。
     */
    [[nodiscard]] voicelife::Status Initialize();

    /**
     * @brief 按受控 asset_id 加载 GIF 资源。
     *
     * 非法格式返回 kInvalidArgument，未知资源返回 kNotFound，
     * 未初始化/不支持返回 kUnavailable。
     * @param asset_id 受控资源标识。
     * @return 资源视图。
     */
    [[nodiscard]] voicelife::Result<GifAssetView> Load(std::string_view asset_id);

   private:
    /** @brief spi_flash_mmap_handle_t（仅 ESP 构建使用）。 */
    [[maybe_unused]] void* mmap_handle_ = nullptr;
    /** @brief mmap 根指针（仅 ESP 构建使用）。 */
    [[maybe_unused]] const void* mmap_root_ = nullptr;
    /** @brief 是否已成功初始化。 */
    [[maybe_unused]] bool initialized_ = false;
};

}  // namespace voicelife::display_sparkbot
