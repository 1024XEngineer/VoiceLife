#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "voicelife/contracts/status.h"

namespace voicelife::platform {

/** @brief 编译期 Profile 中使用的稳定能力标识。 */
using CapabilityId = std::string;

/** @brief 一个板卡或平台能够提供的去重能力集合。 */
class CapabilitySet {
   public:
    /** @brief 创建能力集合。 @param values 声明的能力；重复项会由 Validate() 拒绝。 */
    explicit CapabilitySet(std::vector<CapabilityId> values = {});

    /** @brief 校验能力标识格式及重复项。 @return 合法返回 Ok。 */
    [[nodiscard]] Status Validate() const;

    /** @brief 查询一项能力。 @param capability 稳定能力标识。 @return 是否存在。 */
    [[nodiscard]] bool Has(std::string_view capability) const;

    /** @brief 查询是否包含全部能力。 @param required 必需能力。 @return 是否全部满足。 */
    [[nodiscard]] bool SupportsAll(const std::vector<CapabilityId>& required) const;

    /** @brief 返回原始声明顺序，供诊断和装配读取。 @return 不可变能力列表。 */
    [[nodiscard]] const std::vector<CapabilityId>& values() const { return values_; }

   private:
    std::vector<CapabilityId> values_;
};

/** @brief 编译和装配前必须满足的板级资源下限。 */
struct ResourceBudget {
    /** @brief 最小 Flash 字节数。 */
    std::uint32_t flash_bytes = 0;
    /** @brief 最小 PSRAM 字节数；0 表示该 Profile 不依赖 PSRAM。 */
    std::uint32_t psram_bytes = 0;

    /** @brief 校验资源预算。 @return 合法返回 Ok。 */
    [[nodiscard]] Status Validate() const;
};

/**
 * @brief 一个由构建选择的板卡/平台身份与能力声明。
 *
 * 该结构不携带 GPIO、ESP-IDF 句柄、像素缓冲或凭据。硬件拓扑仍属于
 * 对应 Board Adapter，Profile 只为装配和构建准入提供稳定语义。
 */
struct PlatformProfile {
    /** @brief 稳定 Profile ID。 */
    std::string id;
    /** @brief 板卡 SKU 或平台标识。 */
    std::string board_id;
    /** @brief 可比较的板卡修订标识。 */
    std::string board_revision;
    /** @brief 编译目标，例如 esp32s3。 */
    std::string target;
    /** @brief 平台对 Runtime 承诺的能力。 */
    CapabilitySet capabilities;
    /** @brief 此 Profile 需要的最低资源。 */
    ResourceBudget resource_budget;

    /** @brief 校验身份、能力和资源预算。 @return 合法返回 Ok。 */
    [[nodiscard]] Status Validate() const;
};

/**
 * @brief 平台装配结果的窄生命周期边界。
 *
 * 第一个重构阶段只定义该边界；Runtime 继续使用已验证的 VoiceLife PCB
 * 组装路径，直到每个 Board Adapter 完成独立真机回归。
 */
class PlatformAssembly {
   public:
    /** @brief 虚析构函数。 */
    virtual ~PlatformAssembly() = default;

    /** @brief 返回实际装配的不可变 Profile。 @return 已选择的平台 Profile。 */
    [[nodiscard]] virtual const PlatformProfile& profile() const = 0;

    /** @brief 校验装配所需资源与能力。 @return 可用返回 Ok。 */
    [[nodiscard]] virtual Status Validate() const = 0;
};

/** @brief 由编译期注册的 Board Adapter 实现的平台装配工厂。 */
class PlatformAssemblyFactory {
   public:
    /** @brief 虚析构函数。 */
    virtual ~PlatformAssemblyFactory() = default;

    /**
     * @brief 为已选择的 Profile 创建装配结果。
     * @param profile 构建工具已验证的 Profile。
     * @return 未注册、能力不足或资源不足时返回类型化失败。
     */
    virtual Result<std::unique_ptr<PlatformAssembly>> Create(const PlatformProfile& profile) const = 0;
};

}  // namespace voicelife::platform
