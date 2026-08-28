#include "voicelife/board_esp/sparkbot_imu.h"

#include <limits>

#include "support/test_support.h"

using voicelife::board_esp::SparkBotImu;
using voicelife::board_esp::SparkBotImuSample;
using voicelife::board_esp::SparkBotMotionDetector;
using voicelife::test::Check;

namespace {

SparkBotImuSample StaticSample(float x = 0.0F, float y = 0.0F, float z = 9.80665F, float gyro = 0.0F) {
    return {{x, y, z}, {0.0F, 0.0F, gyro}};
}

SparkBotImuSample DynamicSample(float z, float gyro = 500.0F) { return {{0.0F, 0.0F, z}, {0.0F, 0.0F, gyro}}; }

void FeedRest(SparkBotMotionDetector& detector, uint64_t* timestamp_ms, int count = 1) {
    for (int i = 0; i < count; ++i) {
        Check(!detector.Update(StaticSample(), *timestamp_ms), "静止样本不得触发摇晃");
        *timestamp_ms += 10;
    }
}

}  // namespace

int main() {
    SparkBotMotionDetector detector;
    uint64_t timestamp_ms = 100;
    FeedRest(detector, &timestamp_ms);

    // 非有限传感器值必须被丢弃，并清空半成品摇晃序列。
    const float nan = std::numeric_limits<float>::quiet_NaN();
    Check(!detector.Update(StaticSample(nan), timestamp_ms), "非有限加速度不得触发摇晃");
    Check(!detector.Update(StaticSample(0.0F, nan), timestamp_ms), "非有限加速度分量不得触发摇晃");
    Check(!detector.Update(StaticSample(0.0F, 0.0F, nan), timestamp_ms), "非有限加速度 z 分量不得触发摇晃");
    Check(!detector.Update(StaticSample(0.0F, 0.0F, 9.80665F, nan), timestamp_ms), "非有限角速度不得触发摇晃");

    detector.Reset();
    Check(!detector.Update(StaticSample(0.0F, 0.0F, 0.0F), timestamp_ms), "异常重力基线不得建立参考");

    // 轻微翻转只有姿态变化/低角速度，不应触发。
    for (int i = 0; i < 12; ++i) {
        Check(!detector.Update(StaticSample(9.80665F, 0.0F, 0.0F, 80.0F), timestamp_ms), "慢速翻转不得触发摇晃");
        timestamp_ms += 10;
    }

    // 快速翻转可能产生一个总加速度峰值，但没有交替线性冲量，仍不得触发。
    detector.Reset();
    timestamp_ms = 500;
    FeedRest(detector, &timestamp_ms, 3);
    Check(!detector.Update(StaticSample(9.80665F, 0.0F, 0.0F, 700.0F), timestamp_ms), "快速翻转首个姿态峰不得触发");
    timestamp_ms += 10;
    FeedRest(detector, &timestamp_ms, 12);
    Check(!detector.Update(StaticSample(0.0F, 0.0F, 9.80665F, 0.0F), timestamp_ms), "快速翻转回稳不得触发");

    // 单次碰撞不得触发，即使线性加速度和角速度都很高。
    detector.Reset();
    timestamp_ms = 900;
    FeedRest(detector, &timestamp_ms);
    Check(!detector.Update(StaticSample(8.0F, 0.0F, 9.80665F, 800.0F), timestamp_ms), "单次碰撞不得触发");
    timestamp_ms += 10;
    FeedRest(detector, &timestamp_ms, 15);

    // 有意摇晃表现为三个交替方向的线性加速度脉冲。
    detector.Reset();
    timestamp_ms = 1200;
    FeedRest(detector, &timestamp_ms, 3);
    Check(!detector.Update(DynamicSample(15.0F), timestamp_ms), "第一脉冲只建立摇晃序列");
    timestamp_ms += 10;
    FeedRest(detector, &timestamp_ms, 3);
    Check(!detector.Update(DynamicSample(4.5F), timestamp_ms), "第二脉冲只累计交替计数");
    timestamp_ms += 10;
    FeedRest(detector, &timestamp_ms, 3);
    Check(detector.Update(DynamicSample(15.0F), timestamp_ms), "三个交替线性脉冲应触发摇晃");
    timestamp_ms += 10;
    Check(!detector.Update(DynamicSample(4.5F), timestamp_ms), "触发后冷却期不得重复触发");

    // 新门槛允许幅度较小但仍有明确旋转的真实摇晃；三脉冲约束保持不变。
    detector.Reset();
    timestamp_ms = 1600;
    FeedRest(detector, &timestamp_ms, 3);
    Check(!detector.Update(DynamicSample(13.6F, 75.0F), timestamp_ms), "中等摇晃第一脉冲只累计");
    timestamp_ms += 10;
    FeedRest(detector, &timestamp_ms, 3);
    Check(!detector.Update(DynamicSample(5.9F, 75.0F), timestamp_ms), "中等摇晃第二脉冲只累计");
    timestamp_ms += 10;
    FeedRest(detector, &timestamp_ms, 3);
    Check(detector.Update(DynamicSample(13.6F, 75.0F), timestamp_ms), "中等三脉冲摇晃应触发");

    // 脉冲间隔过长，不能把两次独立动作拼成一次摇晃。
    detector.Reset();
    timestamp_ms = 2000;
    FeedRest(detector, &timestamp_ms);
    Check(!detector.Update(DynamicSample(15.0F), timestamp_ms), "长窗口首脉冲只累计");
    timestamp_ms += 250;
    FeedRest(detector, &timestamp_ms, 2);
    Check(!detector.Update(DynamicSample(4.5F), timestamp_ms), "超时序列不得累计");

    detector.Reset();
    Check(!detector.Update(StaticSample(), 3000), "Reset 后应重新建立重力基线");
    Check(!detector.Update(DynamicSample(15.0F), 2990), "时间戳回退不得产生无符号下溢触发");
    Check(!detector.Update(StaticSample(), 3000), "时间戳回退后应重新等待有效脉冲");

    SparkBotImu imu;
    Check(imu.Start({}).code == voicelife::ErrorCode::kUnavailable, "host 构建不得伪装 IMU 可用");
    Check(!imu.running(), "host 构建启动失败后 running 必须为 false");
    imu.Stop();
    return 0;
}
