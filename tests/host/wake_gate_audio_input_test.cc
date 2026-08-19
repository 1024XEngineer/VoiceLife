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
    Check(physical.starts == 1 && detector.stops == 0, "唤醒命中后检测器已自停，切换上行不应重复停止检测器");
    Check(physical.Emit(Frame()).ok() && detector.frames == 1 && forwarded == 1,
          "上行状态 PCM 必须只转发 VoiceSession");
    Check(gate.StopCapture().ok() && !gate.standby(), "停止上行不得在播报或最终识别期间隐式恢复本地唤醒");
    Check(detector.starts == 1 && physical.starts == 1, "停止上行不得重启物理采集或检测器");
    Check(physical.Emit(Frame()).ok() && detector.frames == 1 && forwarded == 1,
          "非待机阶段 PCM 必须既不继续上行也不送本地检测器");
    Check(gate.StartStandby().ok() && gate.standby(), "只有明确回待机时才恢复本地检测");
    Check(detector.starts == 2 && physical.starts == 1, "恢复待机不得重启物理采集任务");
    Check(physical.Emit(Frame()).ok() && detector.frames == 2 && forwarded == 1, "恢复待机后 PCM 不得继续上行");
    Check(gate.StopCapture().ok() && !gate.standby(), "停止待机检测应使后续中间阶段保持静音门控");
    Check(gate.StartStandby().ok() && gate.standby(), "重复回待机必须恢复检测");
    Check(detector.starts == 3 && physical.starts == 1, "重复待机切换不得创建重复物理采集任务");
    gate.SuppressLocalWakeFor(10'000);
    Check(physical.Emit(Frame()).ok() && detector.frames == 2 && forwarded == 1,
          "终结播报余响窗口内的 PCM 不得送入本地检测器");
    detector.Detect("ni hao niu niu");
    Check(wake_events == 1,
          "终结播报余响窗口必须丢弃检测器中已完成但晚到的唤醒回调");

    gate.Close();
    Check(physical.stops == 1 && physical.closes == 1 && detector.stops == 2,
          "关闭必须停止当前检测、采集并释放物理端口");
    return 0;
}
