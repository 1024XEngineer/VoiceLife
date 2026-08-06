#include <utility>

#include "voicelife/voice/voice_ports.h"

namespace voicelife::voice {

SpeechProviderRegistry& SpeechProviderRegistry::Instance() {
    static SpeechProviderRegistry registry;
    return registry;
}

Status SpeechProviderRegistry::Register(std::string provider_id, CapabilityProfile profile,
                                        SpeechProviderFactory factory) {
    if (provider_id.empty() || !factory || profile.provider_id != provider_id) {
        return Status::Error(ErrorCode::kInvalidArgument, "语音 Provider 注册信息无效");
    }
    for (std::size_t index = 0; index < size_; ++index) {
        if (entries_[index].provider_id == provider_id) {
            return Status::Error(ErrorCode::kAlreadyExists, "语音 Provider 已注册");
        }
    }
    if (size_ == kMaxProviders) {
        return Status::Error(
            ErrorCode::kUnavailable,
            "语音 Provider 注册表已满 (上限 " + std::to_string(kMaxProviders) + ")，请检查是否重复注册或缺少卸载逻辑");
    }
    entries_[size_++] = Entry{std::move(provider_id), std::move(profile), std::move(factory)};
    return Status::Ok();
}

Result<std::unique_ptr<SpeechProviderAdapter>> SpeechProviderRegistry::Create(
    std::string_view provider_id, const std::vector<std::string>& required_capabilities) const {
    const Entry* entry = nullptr;
    for (std::size_t index = 0; index < size_; ++index) {
        if (entries_[index].provider_id == provider_id) {
            entry = &entries_[index];
            break;
        }
    }
    if (entry == nullptr) {
        return Result<std::unique_ptr<SpeechProviderAdapter>>::Failure(ErrorCode::kNotFound, "语音 Provider 未注册");
    }
    for (const std::string& capability : required_capabilities) {
        if (!entry->profile.Has(capability)) {
            return Result<std::unique_ptr<SpeechProviderAdapter>>::Failure(ErrorCode::kUnavailable,
                                                                           "语音 Provider 缺少所需能力: " + capability);
        }
    }
    std::unique_ptr<SpeechProviderAdapter> provider = entry->factory();
    if (!provider) {
        return Result<std::unique_ptr<SpeechProviderAdapter>>::Failure(ErrorCode::kInternal,
                                                                       "语音 Provider 工厂返回空实现");
    }
    return Result<std::unique_ptr<SpeechProviderAdapter>>::Success(std::move(provider));
}

}  // namespace voicelife::voice
