#include "voicelife/platform/platform_profile.h"

#include <algorithm>
#include <cctype>
#include <utility>

namespace voicelife::platform {
namespace {

Status Invalid(std::string message) { return Status::Error(ErrorCode::kInvalidArgument, std::move(message)); }

bool ValidProfileIdentifier(std::string_view value) {
    if (value.empty() || (!std::islower(static_cast<unsigned char>(value.front())) &&
                          !std::isdigit(static_cast<unsigned char>(value.front())))) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](unsigned char character) {
        return std::islower(character) || std::isdigit(character) || character == '.' || character == '-';
    });
}

bool ValidCapabilityIdentifier(std::string_view value) {
    return ValidProfileIdentifier(value) ||
           (value.size() > 1 && std::islower(static_cast<unsigned char>(value.front())) &&
            std::all_of(value.begin(), value.end(), [](unsigned char character) {
                return std::islower(character) || std::isdigit(character) || character == '.' || character == '_' ||
                       character == '-';
            }));
}

}  // namespace

CapabilitySet::CapabilitySet(std::vector<CapabilityId> values) : values_(std::move(values)) {}

Status CapabilitySet::Validate() const {
    std::vector<CapabilityId> sorted = values_;
    for (const CapabilityId& value : sorted) {
        if (!ValidCapabilityIdentifier(value)) {
            return Invalid("平台能力标识格式错误: " + value);
        }
    }
    std::sort(sorted.begin(), sorted.end());
    if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end()) {
        return Invalid("平台能力不能重复");
    }
    return Status::Ok();
}

bool CapabilitySet::Has(std::string_view capability) const {
    return std::any_of(values_.begin(), values_.end(),
                       [capability](const CapabilityId& value) { return value == capability; });
}

bool CapabilitySet::SupportsAll(const std::vector<CapabilityId>& required) const {
    return std::all_of(required.begin(), required.end(),
                       [this](const CapabilityId& capability) { return Has(capability); });
}

Status ResourceBudget::Validate() const {
    if (flash_bytes == 0) {
        return Invalid("平台资源预算必须声明 Flash");
    }
    return Status::Ok();
}

Status PlatformProfile::Validate() const {
    if (!ValidProfileIdentifier(id) || !ValidProfileIdentifier(board_id) || !ValidProfileIdentifier(board_revision) ||
        !ValidProfileIdentifier(target)) {
        return Invalid("平台 Profile 身份字段格式错误");
    }
    const Status capabilities_status = capabilities.Validate();
    if (!capabilities_status.ok()) {
        return capabilities_status;
    }
    return resource_budget.Validate();
}

}  // namespace voicelife::platform
