#include "voicelife/board_esp/gpio46_power_arbiter.h"

namespace voicelife::board_esp {

Gpio46PowerArbiter::Gpio46PowerArbiter(SharedPowerProfile profile) : profile_(profile) {}

Status Gpio46PowerArbiter::Validate() const {
    if (profile_.gpio != 46 || !profile_.backlight_shared || !profile_.audio_output_shared) {
        return Status::Error(ErrorCode::kInvalidArgument, "GPIO46 仲裁器必须绑定 SparkBot 功放/背光共享线");
    }
    return Status::Ok();
}

Status Gpio46PowerArbiter::SetBacklightEnabled(bool enabled) {
    const Status status = Validate();
    if (!status.ok()) {
        return status;
    }
    backlight_enabled_ = enabled;
    return Status::Ok();
}

Status Gpio46PowerArbiter::SetAudioOutputEnabled(bool enabled) {
    const Status status = Validate();
    if (!status.ok()) {
        return status;
    }
    audio_output_enabled_ = enabled;
    return Status::Ok();
}

SharedPowerState Gpio46PowerArbiter::state() const {
    const bool requested = backlight_enabled_ || audio_output_enabled_;
    return {.backlight_enabled = backlight_enabled_,
            .audio_output_enabled = audio_output_enabled_,
            .line_level = requested ? profile_.active_high : !profile_.active_high};
}

bool Gpio46PowerArbiter::line_enabled() const { return backlight_enabled_ || audio_output_enabled_; }

bool Gpio46PowerArbiter::idle_safe() const { return !backlight_enabled_ && !audio_output_enabled_; }

}  // namespace voicelife::board_esp
