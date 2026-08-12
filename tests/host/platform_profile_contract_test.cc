#include <memory>
#include <utility>

#include "support/test_support.h"
#include "voicelife/platform/platform_profile.h"

using voicelife::ErrorCode;
using voicelife::Result;
using voicelife::Status;
using voicelife::test::Check;

namespace {

using voicelife::platform::CapabilitySet;
using voicelife::platform::PlatformAssembly;
using voicelife::platform::PlatformAssemblyFactory;
using voicelife::platform::PlatformProfile;

class TestAssembly final : public PlatformAssembly {
   public:
    explicit TestAssembly(PlatformProfile profile) : profile_(std::move(profile)) {}

    [[nodiscard]] const PlatformProfile& profile() const override { return profile_; }
    [[nodiscard]] Status Validate() const override { return profile_.Validate(); }

   private:
    PlatformProfile profile_;
};

class TestFactory final : public PlatformAssemblyFactory {
   public:
    Result<std::unique_ptr<PlatformAssembly>> Create(const PlatformProfile& profile) const override {
        if (const Status status = profile.Validate(); !status.ok()) {
            return Result<std::unique_ptr<PlatformAssembly>>::Failure(status.code, status.message);
        }
        if (profile.board_id != "test-board") {
            return Result<std::unique_ptr<PlatformAssembly>>::Failure(ErrorCode::kNotFound, "没有匹配的平台装配器");
        }
        return Result<std::unique_ptr<PlatformAssembly>>::Success(std::make_unique<TestAssembly>(profile));
    }
};

PlatformProfile ValidProfile() {
    return {.id = "test-board-profile",
            .board_id = "test-board",
            .board_revision = "r1.0",
            .target = "esp32s3",
            .capabilities = CapabilitySet({"audio", "display"}),
            .resource_budget = {.flash_bytes = 16U * 1024U * 1024U, .psram_bytes = 8U * 1024U * 1024U}};
}

}  // namespace

int main() {
    const CapabilitySet capabilities({"audio", "display"});
    Check(capabilities.Validate().ok(), "不同的平台能力应通过校验");
    Check(capabilities.Has("display") && !capabilities.Has("camera"), "能力集合必须精确查询声明项");
    Check(capabilities.SupportsAll({"audio", "display"}) && !capabilities.SupportsAll({"audio", "camera"}),
          "能力集合必须拒绝未声明的必需能力");
    Check(CapabilitySet({"audio", "audio"}).Validate().code == ErrorCode::kInvalidArgument, "重复的平台能力必须拒绝");
    Check(CapabilitySet({"audio_input"}).Validate().ok(), "能力标识可以使用下划线");
    Check(CapabilitySet({""}).Validate().code == ErrorCode::kInvalidArgument, "空能力标识必须拒绝");
    Check(CapabilitySet({"Audio"}).Validate().code == ErrorCode::kInvalidArgument, "能力标识不能使用大写字母");

    const PlatformProfile profile = ValidProfile();
    Check(profile.Validate().ok(), "完整的平台身份、能力和预算应形成合法 Profile");
    auto missing_id = profile;
    missing_id.id.clear();
    Check(missing_id.Validate().code == ErrorCode::kInvalidArgument, "缺少 Profile ID 必须拒绝");
    auto missing_flash = profile;
    missing_flash.resource_budget.flash_bytes = 0;
    Check(missing_flash.Validate().code == ErrorCode::kInvalidArgument, "缺少 Flash 预算必须拒绝");
    auto invalid_board = profile;
    invalid_board.board_id = "test_board";
    Check(invalid_board.Validate().code == ErrorCode::kInvalidArgument, "板卡身份不得使用 Schema 未允许的下划线");
    auto invalid_revision = profile;
    invalid_revision.board_revision = "R1";
    Check(invalid_revision.Validate().code == ErrorCode::kInvalidArgument, "板卡修订标识不能使用大写字母");
    auto invalid_target = profile;
    invalid_target.target = "esp32_s3";
    Check(invalid_target.Validate().code == ErrorCode::kInvalidArgument, "构建目标不得使用 Schema 未允许的下划线");

    TestFactory factory;
    const auto assembly = factory.Create(profile);
    Check(assembly.ok() && assembly.value.value()->Validate().ok(), "工厂必须交付可校验的指定板卡装配");
    Check(assembly.value.value()->profile().board_revision == "r1.0", "装配不得丢失板卡修订信息");

    auto unsupported = profile;
    unsupported.board_id = "other-board";
    Check(factory.Create(unsupported).status.code == ErrorCode::kNotFound, "未注册板卡不能静默使用其他装配器");
    Check(factory.Create(missing_flash).status.code == ErrorCode::kInvalidArgument,
          "工厂必须在选择装配器前拒绝无效 Profile");
    return 0;
}
