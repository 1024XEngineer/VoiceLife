#include "voicelife/board_esp/sparkbot_imu.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>

#ifdef ESP_PLATFORM
#include <algorithm>
#include <array>
#include <cstring>

#include "bmi270.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#endif

namespace voicelife::board_esp {

bool SparkBotMotionDetector::Update(SparkBotImuSample sample, uint64_t timestamp_ms) {
    const SparkBotAcceleration acceleration = sample.acceleration;
    const SparkBotGyroscope gyroscope = sample.gyroscope;
    const float angular_rate =
        std::sqrt(gyroscope.x * gyroscope.x + gyroscope.y * gyroscope.y + gyroscope.z * gyroscope.z);
    if (!std::isfinite(acceleration.x) || !std::isfinite(acceleration.y) || !std::isfinite(acceleration.z) ||
        !std::isfinite(angular_rate)) {
        pulse_count_ = 0;
        last_pulse_direction_ = 0.0F;
        pulse_armed_ = true;
        return false;
    }
    if (!has_gravity_) {
        const float magnitude = std::sqrt(acceleration.x * acceleration.x + acceleration.y * acceleration.y +
                                          acceleration.z * acceleration.z);
        // A startup or handoff impulse must not become the gravity reference.
        if (std::fabs(magnitude - kNominalGravityMps2) > kBaselineToleranceMps2) return false;
        gravity_ = acceleration;
        has_gravity_ = true;
        has_timestamp_ = true;
        last_timestamp_ms_ = timestamp_ms;
        return false;
    }
    if (has_timestamp_ && timestamp_ms < last_timestamp_ms_) {
        pulse_count_ = 0;
        last_pulse_direction_ = 0.0F;
        pulse_armed_ = true;
        last_timestamp_ms_ = timestamp_ms;
        return false;
    }
    if (has_timestamp_ && timestamp_ms - last_timestamp_ms_ > kMaxMotionWindowMs) {
        pulse_count_ = 0;
        last_pulse_direction_ = 0.0F;
        pulse_armed_ = true;
    }
    has_timestamp_ = true;
    last_timestamp_ms_ = timestamp_ms;
    if (timestamp_ms < cooldown_until_ms_) {
        pulse_count_ = 0;
        last_pulse_direction_ = 0.0F;
        pulse_armed_ = true;
        return false;
    }

    // A vector gravity estimate separates orientation changes from linear force.
    gravity_.x += kGravityAlpha * (acceleration.x - gravity_.x);
    gravity_.y += kGravityAlpha * (acceleration.y - gravity_.y);
    gravity_.z += kGravityAlpha * (acceleration.z - gravity_.z);
    const SparkBotAcceleration linear{
        acceleration.x - gravity_.x,
        acceleration.y - gravity_.y,
        acceleration.z - gravity_.z,
    };
    const float linear_magnitude = std::sqrt(linear.x * linear.x + linear.y * linear.y + linear.z * linear.z);
    const float acceleration_magnitude =
        std::sqrt(acceleration.x * acceleration.x + acceleration.y * acceleration.y + acceleration.z * acceleration.z);
    const float dynamic_acceleration = acceleration_magnitude - kNominalGravityMps2;
    if (!std::isfinite(linear_magnitude) || !std::isfinite(dynamic_acceleration)) {
        pulse_count_ = 0;
        last_pulse_direction_ = 0.0F;
        pulse_armed_ = true;
        return false;
    }

    // A low-force valley arms the next peak, preventing one sustained impulse
    // from being counted repeatedly.
    if (linear_magnitude <= kLinearAccelerationRearmMps2 ||
        std::fabs(dynamic_acceleration) <= kDynamicAccelerationRearmMps2) {
        pulse_armed_ = true;
    }
    if (pulse_count_ != 0 && timestamp_ms - motion_started_ms_ > kMaxMotionWindowMs) {
        pulse_count_ = 0;
        last_pulse_direction_ = 0.0F;
        pulse_armed_ = true;
    }
    if (pulse_count_ != 0 && timestamp_ms - last_pulse_ms_ > kMaxPulseGapMs) {
        pulse_count_ = 0;
        last_pulse_direction_ = 0.0F;
        pulse_armed_ = true;
    }
    if (linear_magnitude < kLinearAccelerationThresholdMps2 ||
        std::fabs(dynamic_acceleration) < kDynamicAccelerationThresholdMps2 ||
        angular_rate < kAngularRateThresholdDps || !pulse_armed_) {
        return false;
    }

    const bool is_reversal = pulse_count_ == 0 || (timestamp_ms - last_pulse_ms_ >= kMinPulseGapMs &&
                                                   dynamic_acceleration * last_pulse_direction_ < 0.0F);
    if (!is_reversal) return false;
    if (pulse_count_ == 0) motion_started_ms_ = timestamp_ms;
    ++pulse_count_;
    last_pulse_direction_ = dynamic_acceleration;
    last_pulse_ms_ = timestamp_ms;
    pulse_armed_ = false;
    if (pulse_count_ < kRequiredAlternatingPulses) return false;

    pulse_count_ = 0;
    pulse_armed_ = true;
    cooldown_until_ms_ = timestamp_ms + kCooldownMs;
    return true;
}

void SparkBotMotionDetector::Reset() {
    has_gravity_ = false;
    gravity_ = {};
    last_pulse_direction_ = 0.0F;
    pulse_count_ = 0;
    pulse_armed_ = true;
    motion_started_ms_ = 0;
    last_pulse_ms_ = 0;
    has_timestamp_ = false;
    last_timestamp_ms_ = 0;
    cooldown_until_ms_ = 0;
}

#ifdef ESP_PLATFORM
namespace {

constexpr char kTag[] = "sparkbot_imu";
constexpr uint8_t kI2cAddress = 0x68;
constexpr uint32_t kI2cClockHz = 400000;
constexpr uint32_t kPollIntervalMs = 10;
constexpr uint8_t kExpectedChipId = 0x24;
constexpr uint16_t kReadWriteLength = 46;
constexpr float kGravityMps2 = 9.80665F;
constexpr uint32_t kMotionLogIntervalMs = 40;
constexpr float kMotionLogAngularRateDps = 50.0F;
constexpr float kMotionLogMagnitudeDeviationMps2 = 1.5F;
// Ignore BMI270 settling transients after power/reset before establishing the
// motion detector baseline. At the 100 Hz task cadence this is about one second.
constexpr uint32_t kWarmupSamples = 100;

struct HardwareContext {
    i2c_master_dev_handle_t device = nullptr;
    bmi2_dev sensor{};
};

int8_t ReadRegister(uint8_t register_address, uint8_t* data, uint32_t length, void* interface_pointer) {
    if (interface_pointer == nullptr || data == nullptr) return BMI2_E_NULL_PTR;
    auto* device = static_cast<i2c_master_dev_handle_t>(interface_pointer);
    const esp_err_t result = i2c_master_transmit_receive(device, &register_address, 1, data, length, 100);
    return result == ESP_OK ? BMI2_OK : BMI2_E_COM_FAIL;
}

int8_t WriteRegister(uint8_t register_address, const uint8_t* data, uint32_t length, void* interface_pointer) {
    if (interface_pointer == nullptr || (data == nullptr && length != 0)) return BMI2_E_NULL_PTR;
    auto* device = static_cast<i2c_master_dev_handle_t>(interface_pointer);
    std::array<uint8_t, BMI2_MAX_LEN + 1> packet{};
    if (length > BMI2_MAX_LEN) return BMI2_E_OUT_OF_RANGE;
    packet[0] = register_address;
    if (length != 0) std::memcpy(packet.data() + 1, data, length);
    const esp_err_t result = i2c_master_transmit(device, packet.data(), length + 1, 100);
    return result == ESP_OK ? BMI2_OK : BMI2_E_COM_FAIL;
}

void DelayMicroseconds(uint32_t period, void*) {
    const uint32_t milliseconds = std::max<uint32_t>(1, (period + 999) / 1000);
    vTaskDelay(pdMS_TO_TICKS(milliseconds));
}

bool ConfigureSensor(bmi2_dev* sensor) {
    if (bmi270_init(sensor) != BMI2_OK) return false;
    bmi2_sens_config config[2]{};
    config[0].type = BMI2_ACCEL;
    config[1].type = BMI2_GYRO;
    if (bmi2_get_sensor_config(config, 2, sensor) != BMI2_OK) return false;
    config[0].cfg.acc.odr = BMI2_ACC_ODR_200HZ;
    config[0].cfg.acc.range = BMI2_ACC_RANGE_2G;
    config[0].cfg.acc.bwp = BMI2_ACC_NORMAL_AVG4;
    config[0].cfg.acc.filter_perf = BMI2_PERF_OPT_MODE;
    config[1].cfg.gyr.odr = BMI2_GYR_ODR_200HZ;
    config[1].cfg.gyr.range = BMI2_GYR_RANGE_2000;
    config[1].cfg.gyr.bwp = BMI2_GYR_NORMAL_MODE;
    config[1].cfg.gyr.noise_perf = BMI2_POWER_OPT_MODE;
    config[1].cfg.gyr.filter_perf = BMI2_PERF_OPT_MODE;
    if (bmi2_set_sensor_config(config, 2, sensor) != BMI2_OK) return false;
    const uint8_t sensors[] = {BMI2_ACCEL, BMI2_GYRO};
    return bmi2_sensor_enable(sensors, 2, sensor) == BMI2_OK;
}

}  // namespace

class SparkBotImu::Impl final {
   public:
    explicit Impl(SparkBotImu& owner) : owner_(owner) {}
    ~Impl() { owner_.Stop(); }

    Status Start(SparkBotImu::ShakeCallback callback) {
        callback_ = std::move(callback);
        if (task_.load() != nullptr) return Status::Ok();
        i2c_master_bus_handle_t bus = nullptr;
        if (i2c_master_get_bus_handle(I2C_NUM_0, &bus) != ESP_OK || bus == nullptr) {
            return Status::Error(ErrorCode::kUnavailable, "SparkBot IMU 依赖的 I2C0 尚未初始化");
        }
        if (i2c_master_probe(bus, kI2cAddress, 50) != ESP_OK) {
            return Status::Error(ErrorCode::kNotFound, "SparkBot BMI270 未在 I2C 地址 0x68 响应");
        }
        const i2c_device_config_t config = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = kI2cAddress,
            .scl_speed_hz = kI2cClockHz,
            .scl_wait_us = 0,
            .flags = {},
        };
        if (i2c_master_bus_add_device(bus, &config, &context_.device) != ESP_OK) {
            return Status::Error(ErrorCode::kUnavailable, "创建 SparkBot BMI270 I2C 设备失败");
        }
        context_.sensor = {};
        context_.sensor.intf = BMI2_I2C_INTF;
        context_.sensor.intf_ptr = context_.device;
        context_.sensor.read = ReadRegister;
        context_.sensor.write = WriteRegister;
        context_.sensor.delay_us = DelayMicroseconds;
        context_.sensor.read_write_len = kReadWriteLength;
        if (!ConfigureSensor(&context_.sensor) || context_.sensor.chip_id != kExpectedChipId) {
            ESP_LOGE(kTag, "BMI270_INIT_FAILED chip_id=0x%02X", context_.sensor.chip_id);
            CleanupDevice();
            return Status::Error(ErrorCode::kUnavailable, "SparkBot BMI270 初始化或 chip ID 校验失败");
        }
        stop_requested_.store(false);
        owner_.running_.store(true);
        TaskHandle_t task = nullptr;
        if (xTaskCreateWithCaps(&TaskEntry, "sparkbot_imu", 4096, this, 4, &task,
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
            owner_.running_.store(false);
            CleanupDevice();
            return Status::Error(ErrorCode::kUnavailable, "创建 SparkBot IMU 任务失败");
        }
        task_.store(task);
        ESP_LOGI(kTag,
                 "SPARKBOT_IMU_READY sensor=BMI270 addr=0x%02X chip_id=0x%02X odr_hz=200 poll_ms=%u "
                 "accel_range_g=2 gyro_range_dps=2000",
                 kI2cAddress, context_.sensor.chip_id, kPollIntervalMs);
        return Status::Ok();
    }

    void Stop() {
        stop_requested_.store(true);
        TaskHandle_t task = task_.load();
        if (task != nullptr) xTaskNotifyGive(task);
        for (uint8_t attempt = 0; attempt < 50 && task_.load() != nullptr; ++attempt) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (task_.load() == nullptr) CleanupDevice();
    }

   private:
    static void TaskEntry(void* context) { static_cast<Impl*>(context)->Task(); }

    void Task() {
        uint32_t sample_count = 0;
        bool warmup_reported = false;
        uint64_t last_motion_log_ms = 0;
        while (!stop_requested_.load()) {
            bmi2_sens_data data{};
            const int8_t result = bmi2_get_sensor_data(&data, &context_.sensor);
            if (result == BMI2_OK && (data.status & BMI2_DRDY_ACC) && (data.status & BMI2_DRDY_GYR)) {
                const float scale = 2.0F * 9.80665F / 32768.0F;
                const SparkBotAcceleration acceleration{
                    static_cast<float>(data.acc.x) * scale,
                    static_cast<float>(data.acc.y) * scale,
                    static_cast<float>(data.acc.z) * scale,
                };
                const float gyro_scale = 2000.0F / 32768.0F;
                const SparkBotGyroscope gyroscope{
                    static_cast<float>(data.gyr.x) * gyro_scale,
                    static_cast<float>(data.gyr.y) * gyro_scale,
                    static_cast<float>(data.gyr.z) * gyro_scale,
                };
                const SparkBotImuSample sample{acceleration, gyroscope};
                const uint64_t timestamp_ms = static_cast<uint64_t>(esp_timer_get_time() / 1000);
                ++sample_count;
                bool shake = false;
                if (sample_count <= kWarmupSamples) {
                    detector_.Reset();
                } else {
                    if (!warmup_reported) {
                        ESP_LOGI(kTag, "SPARKBOT_IMU_WARMUP_DONE=1 samples=%u", kWarmupSamples);
                        warmup_reported = true;
                    }
                    shake = detector_.Update(sample, timestamp_ms);
                }
                const float acceleration_magnitude =
                    std::sqrt(acceleration.x * acceleration.x + acceleration.y * acceleration.y +
                              acceleration.z * acceleration.z);
                const float angular_rate =
                    std::sqrt(gyroscope.x * gyroscope.x + gyroscope.y * gyroscope.y + gyroscope.z * gyroscope.z);
                if ((angular_rate >= kMotionLogAngularRateDps ||
                     std::fabs(acceleration_magnitude - kGravityMps2) >= kMotionLogMagnitudeDeviationMps2) &&
                    timestamp_ms - last_motion_log_ms >= kMotionLogIntervalMs) {
                    last_motion_log_ms = timestamp_ms;
                    ESP_LOGI(kTag, "SPARKBOT_IMU_MOTION acc_ms2=%.2f,%.2f,%.2f gyro_dps=%.1f,%.1f,%.1f", acceleration.x,
                             acceleration.y, acceleration.z, gyroscope.x, gyroscope.y, gyroscope.z);
                }
                if (sample_count % 100 == 0) {
                    ESP_LOGI(kTag, "SPARKBOT_IMU_SAMPLE seq=%u acc_ms2=%.2f,%.2f,%.2f gyro_dps=%.1f,%.1f,%.1f",
                             sample_count, acceleration.x, acceleration.y, acceleration.z, gyroscope.x, gyroscope.y,
                             gyroscope.z);
                }
                if (shake) {
                    ESP_LOGI(kTag, "SPARKBOT_IMU_SHAKE detected=1 acc_mag_ms2=%.2f gyro_rate_dps=%.1f",
                             acceleration_magnitude, angular_rate);
                    if (callback_) callback_();
                }
            } else if (result != BMI2_OK) {
                ESP_LOGW(kTag, "SPARKBOT_IMU_READ_FAILED code=%d", static_cast<int>(result));
            }
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(kPollIntervalMs));
        }
        owner_.running_.store(false);
        task_.store(nullptr);
        vTaskDelete(nullptr);
    }

    void CleanupDevice() {
        if (context_.device != nullptr) {
            (void)i2c_master_bus_rm_device(context_.device);
            context_.device = nullptr;
        }
        context_.sensor = {};
    }

    SparkBotImu& owner_;
    HardwareContext context_;
    std::atomic<bool> stop_requested_{false};
    std::atomic<TaskHandle_t> task_{nullptr};
    SparkBotMotionDetector detector_;
    SparkBotImu::ShakeCallback callback_;
};

SparkBotImu::SparkBotImu() : impl_(std::make_unique<Impl>(*this)) {}

SparkBotImu::~SparkBotImu() = default;

Status SparkBotImu::Start(ShakeCallback callback) { return impl_->Start(std::move(callback)); }

void SparkBotImu::Stop() { impl_->Stop(); }

#else

class SparkBotImu::Impl final {};

SparkBotImu::SparkBotImu() : impl_(std::make_unique<Impl>()) {}

SparkBotImu::~SparkBotImu() = default;

Status SparkBotImu::Start(ShakeCallback) {
    return Status::Error(ErrorCode::kUnavailable, "SparkBot IMU 只能在 ESP-IDF 目标运行");
}

void SparkBotImu::Stop() {}

#endif

}  // namespace voicelife::board_esp
