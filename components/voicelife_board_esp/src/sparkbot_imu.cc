#include "voicelife/board_esp/sparkbot_imu.h"

#include <cmath>
#include <string>
#include <utility>

#ifdef ESP_PLATFORM
#include <atomic>
#include <cstring>
#include <vector>

#include "bmi270.h"
#include "bmi2_defs.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

namespace voicelife::board_esp {
namespace {

Status Unavailable(std::string message) { return Status::Error(ErrorCode::kUnavailable, std::move(message)); }

}  // namespace

ShakeDetector::ShakeDetector(float threshold_mps2, uint32_t cooldown_ms)
    : threshold_mps2_(threshold_mps2), cooldown_ms_(cooldown_ms) {}

bool ShakeDetector::Push(ImuAcceleration acceleration, uint64_t timestamp_ms) {
    const float magnitude = std::sqrt(acceleration.x * acceleration.x + acceleration.y * acceleration.y +
                                      acceleration.z * acceleration.z);
    if (!std::isfinite(magnitude)) return false;
    if (!baseline_ready_) {
        baseline_magnitude_mps2_ = magnitude;
        baseline_ready_ = true;
        return false;
    }

    constexpr float kBaselineAlpha = 0.02F;
    baseline_magnitude_mps2_ += kBaselineAlpha * (magnitude - baseline_magnitude_mps2_);
    if (std::fabs(magnitude - baseline_magnitude_mps2_) >= threshold_mps2_) {
        if (active_samples_ < 3) ++active_samples_;
    } else {
        active_samples_ = 0;
    }
    if (active_samples_ < 3 || (event_seen_ && timestamp_ms - last_event_ms_ < cooldown_ms_)) return false;
    active_samples_ = 0;
    event_seen_ = true;
    last_event_ms_ = timestamp_ms;
    return true;
}

void ShakeDetector::Reset() {
    baseline_magnitude_mps2_ = 0.0F;
    active_samples_ = 0;
    last_event_ms_ = 0;
    baseline_ready_ = false;
    event_seen_ = false;
}

#ifdef ESP_PLATFORM

constexpr float kGravityMps2 = 9.80665F;
constexpr uint32_t kSamplePeriodMs = 5;
constexpr uint8_t kBmi270ChipId = 0x24;

class SparkBotImu::Impl final {
   public:
    Impl(uint8_t i2c_port, uint8_t i2c_address) : i2c_port_(i2c_port), i2c_address_(i2c_address) {}

    ~Impl() { Stop(); }

    Status Start(ShakeCallback callback) {
        if (task_.load() != nullptr) return Status::Ok();
        callback_ = std::move(callback);
        i2c_master_bus_handle_t bus = nullptr;
        if (i2c_master_get_bus_handle(static_cast<i2c_port_num_t>(i2c_port_), &bus) != ESP_OK || bus == nullptr) {
            return Unavailable("SparkBot BMI270 需要已初始化的 I2C0 总线");
        }

        const uint8_t addresses[] = {i2c_address_, static_cast<uint8_t>(i2c_address_ == 0x68 ? 0x69 : 0x68)};
        bool found = false;
        for (const uint8_t address : addresses) {
            if (i2c_master_probe(bus, address, 50) == ESP_OK) {
                i2c_address_ = address;
                found = true;
                break;
            }
        }
        if (!found) return Unavailable("未探测到 SparkBot BMI270");

        i2c_device_config_t device_config = {};
        device_config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        device_config.device_address = i2c_address_;
        device_config.scl_speed_hz = 400000;
        if (i2c_master_bus_add_device(bus, &device_config, &device_) != ESP_OK) {
            return Unavailable("创建 BMI270 I2C 设备句柄失败");
        }

        std::memset(&bmi_, 0, sizeof(bmi_));
        bmi_.intf = BMI2_I2C_INTF;
        bmi_.intf_ptr = this;
        bmi_.read = &Impl::Read;
        bmi_.write = &Impl::Write;
        bmi_.delay_us = &Impl::Delay;
        bmi_.read_write_len = 64;
        const int8_t init_result = bmi270_init(&bmi_);
        if (init_result != BMI2_OK || bmi_.chip_id != kBmi270ChipId) {
            ESP_LOGW(kTag, "BMI270_INIT_FAILED result=%d chip_id=0x%02X", static_cast<int>(init_result), bmi_.chip_id);
            CleanupDevice();
            return Unavailable("BMI270 初始化失败");
        }

        struct bmi2_sens_config accel = {};
        accel.type = BMI2_ACCEL;
        if (bmi2_get_sensor_config(&accel, 1, &bmi_) != BMI2_OK) {
            CleanupDevice();
            return Unavailable("读取 BMI270 加速度配置失败");
        }
        accel.cfg.acc.odr = BMI2_ACC_ODR_200HZ;
        accel.cfg.acc.range = BMI2_ACC_RANGE_2G;
        accel.cfg.acc.bwp = BMI2_ACC_NORMAL_AVG4;
        accel.cfg.acc.filter_perf = BMI2_PERF_OPT_MODE;
        if (bmi2_set_sensor_config(&accel, 1, &bmi_) != BMI2_OK) {
            CleanupDevice();
            return Unavailable("设置 BMI270 加速度配置失败");
        }
        const uint8_t sensors[] = {BMI2_ACCEL};
        if (bmi2_sensor_enable(sensors, 1, &bmi_) != BMI2_OK) {
            CleanupDevice();
            return Unavailable("启用 BMI270 加速度计失败");
        }

        stop_requested_.store(false);
        TaskHandle_t task = nullptr;
        if (xTaskCreate(&Impl::TaskEntry, "sparkbot_imu", 4096, this, 4, &task) != pdPASS) {
            CleanupDevice();
            return Status::Error(ErrorCode::kInternal, "创建 SparkBot IMU 任务失败");
        }
        task_.store(task);
        ESP_LOGI(kTag, "SPARKBOT_IMU_READY sensor=BMI270 addr=0x%02X sample_hz=200 threshold_mps2=4.5", i2c_address_);
        return Status::Ok();
    }

    void Stop() {
        TaskHandle_t task = task_.load();
        if (task != nullptr) {
            stop_requested_.store(true);
            for (int attempt = 0; attempt < 100 && task_.load() != nullptr; ++attempt) {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }
        CleanupDevice();
    }

    bool running() const { return task_.load() != nullptr; }

   private:
    static constexpr const char* kTag = "sparkbot_imu";

    static int8_t Read(uint8_t reg, uint8_t* data, uint32_t length, void* context) {
        auto* self = static_cast<Impl*>(context);
        if (self == nullptr || self->device_ == nullptr || data == nullptr || length > UINT16_MAX) {
            return BMI2_E_COM_FAIL;
        }
        return i2c_master_transmit_receive(self->device_, &reg, 1, data, length, 100) == ESP_OK ? BMI2_OK
                                                                                                   : BMI2_E_COM_FAIL;
    }

    static int8_t Write(uint8_t reg, const uint8_t* data, uint32_t length, void* context) {
        auto* self = static_cast<Impl*>(context);
        if (self == nullptr || self->device_ == nullptr || (length != 0 && data == nullptr) || length >= 256) {
            return BMI2_E_COM_FAIL;
        }
        std::vector<uint8_t> payload(length + 1, 0);
        payload[0] = reg;
        if (length != 0) std::memcpy(payload.data() + 1, data, length);
        return i2c_master_transmit(self->device_, payload.data(), payload.size(), 100) == ESP_OK ? BMI2_OK
                                                                                                   : BMI2_E_COM_FAIL;
    }

    static void Delay(uint32_t period_us, void*) {
        const TickType_t ticks = pdMS_TO_TICKS((period_us + 999U) / 1000U);
        if (ticks > 0) vTaskDelay(ticks);
    }

    static void TaskEntry(void* context) {
        static_cast<Impl*>(context)->Task();
        vTaskDelete(nullptr);
    }

    void Task() {
        ShakeDetector detector;
        const uint64_t start_ms = esp_timer_get_time() / 1000ULL;
        while (!stop_requested_.load()) {
            struct bmi2_sens_data data = {};
            if (bmi2_get_sensor_data(&data, &bmi_) == BMI2_OK && (data.status & BMI2_DRDY_ACC) != 0) {
                const float scale = (2.0F * kGravityMps2) / static_cast<float>(1U << bmi_.resolution);
                const ImuAcceleration acceleration{.x = static_cast<float>(data.acc.x) * scale,
                                                    .y = static_cast<float>(data.acc.y) * scale,
                                                    .z = static_cast<float>(data.acc.z) * scale};
                const uint64_t now_ms = esp_timer_get_time() / 1000ULL - start_ms;
                if (detector.Push(acceleration, now_ms) && callback_) callback_();
            }
            vTaskDelay(pdMS_TO_TICKS(kSamplePeriodMs));
        }
        task_.store(nullptr);
    }

    void CleanupDevice() {
        if (device_ != nullptr) {
            (void)i2c_master_bus_rm_device(device_);
            device_ = nullptr;
        }
    }

    uint8_t i2c_port_;
    uint8_t i2c_address_;
    i2c_master_dev_handle_t device_ = nullptr;
    struct bmi2_dev bmi_ = {};
    ShakeCallback callback_;
    std::atomic<bool> stop_requested_{false};
    std::atomic<TaskHandle_t> task_{nullptr};
};

#endif  // ESP_PLATFORM

#ifndef ESP_PLATFORM
class SparkBotImu::Impl final {};
#endif

SparkBotImu::SparkBotImu(uint8_t i2c_port, uint8_t i2c_address)
#ifdef ESP_PLATFORM
    : impl_(std::make_unique<Impl>(i2c_port, i2c_address))
#else
    : impl_(nullptr)
#endif
{
#ifndef ESP_PLATFORM
    (void)i2c_port;
    (void)i2c_address;
#endif
}

SparkBotImu::~SparkBotImu() {
#ifdef ESP_PLATFORM
    if (impl_ != nullptr) impl_->Stop();
#endif
}

Status SparkBotImu::Start(ShakeCallback on_shake) {
#ifdef ESP_PLATFORM
    return impl_->Start(std::move(on_shake));
#else
    (void)on_shake;
    return Unavailable("主机构建不初始化 SparkBot BMI270");
#endif
}

void SparkBotImu::Stop() {
#ifdef ESP_PLATFORM
    if (impl_ != nullptr) impl_->Stop();
#endif
}

bool SparkBotImu::running() const {
#ifdef ESP_PLATFORM
    return impl_ != nullptr && impl_->running();
#else
    return false;
#endif
}

}  // namespace voicelife::board_esp
