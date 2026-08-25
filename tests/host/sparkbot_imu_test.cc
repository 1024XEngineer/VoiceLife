#include "voicelife/board_esp/sparkbot_imu.h"

#include "support/test_support.h"

using voicelife::ErrorCode;
using voicelife::board_esp::ShakeDetector;
using voicelife::board_esp::SparkBotImu;
using voicelife::test::Check;

int main() {
    ShakeDetector detector;
    Check(!detector.Push({0.0F, 0.0F, 9.80665F}, 0), "首次静止采样只能建立基线");
    Check(!detector.Push({0.0F, 0.0F, 9.80665F}, 5), "静止采样不能触发摇晃");
    Check(!detector.Push({0.1F, 0.0F, 9.8F}, 10), "静止噪声不能触发摇晃");

    Check(!detector.Push({0.0F, 0.0F, 20.0F}, 15), "单次冲击不能直接触发摇晃");
    Check(!detector.Push({0.0F, 0.0F, 20.0F}, 20), "连续冲击仍需满足最小样本数");
    Check(detector.Push({0.0F, 0.0F, 20.0F}, 25), "第三个连续冲击样本应触发一次摇晃");
    Check(!detector.Push({0.0F, 0.0F, 20.0F}, 30), "同一冲击窗口不得重复触发");
    Check(!detector.Push({0.0F, 0.0F, 20.0F}, 1000), "冷却时间内不得重复触发");
    Check(detector.Push({0.0F, 0.0F, 20.0F}, 1530), "冷却结束后再次满足阈值应可触发");

    detector.Reset();
    Check(!detector.Push({0.0F, 0.0F, 9.80665F}, 0), "Reset 后应重新建立基线");
    Check(!detector.Push({0.0F, 0.0F, 9.80665F}, 5), "Reset 后静止仍不得触发");
    SparkBotImu imu;
    Check(!imu.running(), "主机构建不能伪造 SparkBot BMI270 已运行");
    Check(imu.Start({}).code == ErrorCode::kUnavailable, "主机构建启动 IMU 必须明确返回不可用");
    imu.Stop();
    Check(!imu.running(), "主机构建停止不可用 IMU 后仍必须保持未运行");
    return 0;
}
