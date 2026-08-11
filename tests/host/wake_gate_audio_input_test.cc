#include "voicelife/voice/wake_gate_audio_input.h"

#include <utility>

#include "support/test_support.h"

using voicelife::ErrorCode;
using voicelife::Status;
using voicelife::test::Check;

namespace {

class FakeInput final : public voicelife::voice::AudioInputPort {
   public:
    void SetAudioSink(voicelife::voice::AudioFrameSink sink) override { sink_ = std::move(sink); }
    Status Open(const voicelife::voice::AudioFormat&) override {
        ++opens;
        return Status::Ok();
    }
    Status StartCapture(voicelife::voice::VoiceMode) override {
        ++starts;
        return Status::Ok();
    }
    Status StopCapture() override {
        ++stops;
        return Status::Ok();
    }
    void Close() override { ++closes; }
    Status Emit(voicelife::voice::AudioFrame frame) {
        return sink_ ? sink_(std::move(frame)) : Status::Error(ErrorCode::kUnavailable, "采集回调未绑定");
    }

    int opens = 0;
    int starts = 0;
    int stops = 0;
    int closes = 0;

   private:
    voicelife::voice::AudioFrameSink sink_;
};

class FakeDetector final : public voicelife::voice::LocalWakeDetectorPort {
   public:
    Status Start(WakeSink sink) override {
        ++starts;
        sink_ = std::move(sink);
        running = true;
        return Status::Ok();
    }
    Status Stop() override {
        ++stops;
        running = false;
        return Status::Ok();
    }
    Status Submit(const voicelife::voice::AudioFrame&) override {
        ++frames;
        return Status::Ok();
    }
    void Detect(std::string_view word) {
        if (running && sink_) sink_(word);
    }

    int starts = 0;
    int stops = 0;
    int frames = 0;
    bool running = false;

   private:
    WakeSink sink_;
};

voicelife::voice::AudioFrame Frame() {
    voicelife::voice::AudioFrame frame;
    frame.payload = {1, 2};
    return frame;
}

}  // namespace

int main() {
    FakeInput physical;
    FakeDetector detector;
    voicelife::voice::WakeGateAudioInput gate(physical, detector);
    int forwarded = 0;
    int wake_events = 0;
    gate.SetAudioSink([&forwarded](voicelife::voice::AudioFrame) {
        ++forwarded;
        return Status::Ok();
    });
    gate.SetWakeSink([&wake_events](std::string_view word) {
        Check(word == "ni hao niu niu", "唤醒回调必须保留检测器给出的词");
        ++wake_events;
    });

    Check(gate.StartStandby().code == ErrorCode::kUnavailable, "未打开的输入端口不能进入待机");
    Check(gate.Open({}).ok() && physical.opens == 1, "门控必须先打开物理输入");
    Check(gate.StartStandby().ok() && gate.standby(), "待机应启动检测器和一次物理采集");
    Check(physical.starts == 1 && detector.starts == 1, "待机不能创建重复 I2S 采集任务");
    Check(physical.Emit(Frame()).ok() && detector.frames == 1 && forwarded == 0, "待机 PCM 只能送本地检测器，不能上行");
    detector.Detect("ni hao niu niu");
    Check(wake_events == 1 && forwarded == 0, "检测事件不能把待机 PCM 误送云端");

    Check(gate.StartCapture(voicelife::voice::VoiceMode::kRealtime).ok(), "唤醒后应能切换到云端采集");
    Check(physical.starts == 1 && detector.stops == 1, "切换上行不能重新打开 I2S，且必须停止检测器");
    Check(physical.Emit(Frame()).ok() && detector.frames == 1 && forwarded == 1,
          "上行状态 PCM 必须只转发 VoiceSession");
    Check(gate.StopCapture().ok() && gate.standby(), "停止上行后必须恢复本地待机");
    Check(detector.starts == 2 && physical.starts == 1, "恢复待机不得重启物理采集任务");
    Check(physical.Emit(Frame()).ok() && detector.frames == 2 && forwarded == 1, "恢复待机后 PCM 不得继续上行");
    Check(gate.StopCapture().ok() && gate.standby(), "重复恢复待机不得使输入门控失去本地检测");
    Check(detector.starts == 2 && physical.starts == 1, "已在待机时恢复不得创建重复检测或采集任务");

    gate.Close();
    Check(physical.stops == 1 && physical.closes == 1 && detector.stops == 2, "关闭必须停止检测、采集并释放物理端口");
    return 0;
}
