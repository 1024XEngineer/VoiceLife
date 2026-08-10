#include "voicelife/voice/wake_gate_audio_input.h"

#include <utility>

namespace voicelife::voice {
namespace {

Status Unavailable(std::string message) { return Status::Error(ErrorCode::kUnavailable, std::move(message)); }

}  // namespace

WakeGateAudioInput::WakeGateAudioInput(AudioInputPort& physical_input, LocalWakeDetectorPort& detector)
    : physical_input_(physical_input), detector_(detector) {}

void WakeGateAudioInput::SetWakeSink(WakeSink sink) {
    std::lock_guard<std::mutex> lock(mutex_);
    wake_sink_ = std::move(sink);
}

void WakeGateAudioInput::SetAudioSink(AudioFrameSink sink) {
    std::lock_guard<std::mutex> lock(mutex_);
    audio_sink_ = std::move(sink);
}

Status WakeGateAudioInput::Open(const AudioFormat& format) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (opened_) return Status::Ok();
    const Status status = physical_input_.Open(format);
    if (!status.ok()) return status;
    physical_input_.SetAudioSink([this](AudioFrame frame) {
        HandlePhysicalFrame(std::move(frame));
        return Status::Ok();
    });
    opened_ = true;
    return Status::Ok();
}

Status WakeGateAudioInput::StartDetectorLocked() {
    if (detector_running_) return Status::Ok();
    const Status status = detector_.Start([this](std::string_view wake_word) { HandleWakeWord(wake_word); });
    if (status.ok()) detector_running_ = true;
    return status;
}

Status WakeGateAudioInput::StopDetectorLocked() {
    if (!detector_running_) return Status::Ok();
    const Status status = detector_.Stop();
    if (status.ok()) detector_running_ = false;
    return status;
}

Status WakeGateAudioInput::StartStandby() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!opened_) return Unavailable("本地待机前必须先打开输入端口");
    if (forwarding_) return Unavailable("云端采集进行中，不能进入本地待机");
    Status status = StartDetectorLocked();
    if (!status.ok()) return status;
    if (physical_running_) return Status::Ok();
    status = physical_input_.StartCapture(VoiceMode::kAuto);
    if (!status.ok()) {
        (void)StopDetectorLocked();
        return status;
    }
    physical_running_ = true;
    return Status::Ok();
}

Status WakeGateAudioInput::StartCapture(VoiceMode mode) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!opened_) return Unavailable("云端采集前必须先打开输入端口");
    if (forwarding_) return Status::Ok();
    const Status stop_status = StopDetectorLocked();
    if (!stop_status.ok()) return stop_status;
    if (!physical_running_) {
        const Status status = physical_input_.StartCapture(mode);
        if (!status.ok()) {
            (void)StartDetectorLocked();
            return status;
        }
        physical_running_ = true;
    }
    forwarding_ = true;
    return Status::Ok();
}

Status WakeGateAudioInput::StopCapture() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!opened_) return Status::Ok();
    forwarding_ = false;
    return StartDetectorLocked();
}

void WakeGateAudioInput::Close() {
    std::lock_guard<std::mutex> lock(mutex_);
    forwarding_ = false;
    (void)StopDetectorLocked();
    if (physical_running_) {
        (void)physical_input_.StopCapture();
        physical_running_ = false;
    }
    physical_input_.SetAudioSink({});
    physical_input_.Close();
    audio_sink_ = {};
    wake_sink_ = {};
    opened_ = false;
}

bool WakeGateAudioInput::standby() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return opened_ && physical_running_ && detector_running_ && !forwarding_;
}

void WakeGateAudioInput::HandlePhysicalFrame(AudioFrame frame) {
    AudioFrameSink audio_sink;
    LocalWakeDetectorPort* detector = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (forwarding_) {
            audio_sink = audio_sink_;
        } else if (detector_running_) {
            detector = &detector_;
        }
    }
    if (audio_sink) {
        (void)audio_sink(std::move(frame));
    } else if (detector != nullptr) {
        (void)detector->Submit(frame);
    }
}

void WakeGateAudioInput::HandleWakeWord(std::string_view wake_word) {
    WakeSink sink;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (forwarding_ || !detector_running_) return;
        sink = wake_sink_;
    }
    if (sink) sink(wake_word);
}

}  // namespace voicelife::voice
