#include "platform_assemblies.h"

#include "voicelife/audio_esp/sparkbot_audio_budget.h"
#include "voicelife/board_esp/sparkbot_profile.h"
#include "voicelife/display_sparkbot/sparkbot_lvgl_display.h"

#ifdef ESP_PLATFORM
#include <driver/gpio.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <led_strip.h>

namespace {
constexpr const char* kPowerTag = "sparkbot_power";
}
#endif

namespace voicelife::runtime {

namespace {

/** @brief 从官方 SparkBot 板级 Profile 填充 LVGL 显示配置。 */
voicelife::display_sparkbot::SparkBotLcdConfig MakeSparkBotLcdConfig() {
    const auto profile = voicelife::board_esp::SparkBotProfile();
    voicelife::display_sparkbot::SparkBotLcdConfig config;
    config.spi_host = profile.display.spi_host;
    config.spi_mode = profile.display.spi_mode;
    config.pixel_clock_hz = profile.display.pixel_clock_hz;
    config.dc_gpio = profile.display.dc_gpio;
    config.cs_gpio = profile.display.cs_gpio;
    config.clk_gpio = profile.display.clk_gpio;
    config.mosi_gpio = profile.display.mosi_gpio;
    config.reset_gpio = profile.display.reset_gpio;
    config.width = profile.display.width;
    config.height = profile.display.height;
    config.offset_x = profile.display.offset_x;
    config.offset_y = profile.display.offset_y;
    config.mirror_x = profile.display.mirror_x;
    config.mirror_y = profile.display.mirror_y;
    config.swap_xy = profile.display.swap_xy;
    return config;
}

}  // namespace

VoiceLifePcbAssembly::VoiceLifePcbAssembly() : audio_ports_(audio_esp::VoiceLifePcbEsp32s3Profile()) {
    wake_detector_ = std::make_unique<audio_esp::EspMultiNetWakeDetector>();
    wake_gate_ = std::make_unique<voice::WakeGateAudioInput>(audio_ports_.input(), *wake_detector_, true);
}

voicelife::voice::PresentationPort& VoiceLifePcbAssembly::presentation() { return ssd1306_adapter_; }

voicelife::Status VoiceLifePcbAssembly::Start() { return ssd1306_adapter_.Start(); }

void VoiceLifePcbAssembly::BoardInputTaskEntry(void* context) {
    static_cast<VoiceLifePcbAssembly*>(context)->BoardInputTask();
}

voicelife::Status VoiceLifePcbAssembly::StartGpioInput(std::array<int, 4> gpios, BoardInputSink sink,
                                                       const char* task_name) {
    board_input_sink_ = std::move(sink);
#ifdef ESP_PLATFORM
    uint64_t pin_mask = 0;
    for (const int gpio : gpios) {
        if (gpio >= 0) pin_mask |= 1ULL << static_cast<unsigned>(gpio);
    }
    const gpio_config_t config = {
        .pin_bit_mask = pin_mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&config) != ESP_OK) {
        return Status::Error(ErrorCode::kUnavailable, "初始化 PCB 按键 GPIO 失败");
    }
    button_count_ = gpios.size();
    for (std::size_t index = 0; index < button_count_; ++index) {
        buttons_[index].gpio = gpios[index];
        buttons_[index].previous_pressed = false;
        buttons_[index].long_fired = false;
        buttons_[index].pressed_at_us = 0;
    }
    if (xTaskCreate(&VoiceLifePcbAssembly::BoardInputTaskEntry, task_name, 3072, this, 5, nullptr) != pdPASS) {
        return Status::Error(ErrorCode::kInternal, "创建 PCB 按键任务失败");
    }
#else
    (void)gpios;
    (void)task_name;
#endif
    return Status::Ok();
}

voicelife::Status VoiceLifePcbAssembly::StartBoardInput(BoardInputSink sink) {
    return StartGpioInput({0, 47, 40, 39}, std::move(sink), "voicelife_buttons");
}

void VoiceLifePcbAssembly::BoardInputTask() {
#ifdef ESP_PLATFORM
    constexpr int64_t kLongPressUs = 2 * 1000 * 1000;
    while (true) {
        const int64_t now = esp_timer_get_time();
        for (std::size_t index = 0; index < button_count_; ++index) {
            auto& button = buttons_[index];
            const bool pressed = gpio_get_level(static_cast<gpio_num_t>(button.gpio)) == 0;
            if (pressed && !button.previous_pressed) {
                button.pressed_at_us = now;
                button.long_fired = false;
                if (index == 1 && board_input_sink_) board_input_sink_(BoardInputAction::kPressDown);
            } else if (pressed && !button.long_fired && now - button.pressed_at_us >= kLongPressUs) {
                button.long_fired = true;
                if (board_input_sink_) {
                    if (index == 0) board_input_sink_(BoardInputAction::kStartWifiProvisioning);
                    if (index == 2) board_input_sink_(BoardInputAction::kVolumeMaximum);
                    if (index == 3) board_input_sink_(BoardInputAction::kVolumeMute);
                }
            } else if (!pressed && button.previous_pressed) {
                if (board_input_sink_) {
                    if (index == 0 && !button.long_fired) board_input_sink_(BoardInputAction::kToggleChat);
                    if (index == 1) board_input_sink_(BoardInputAction::kPressUp);
                    if (index == 2 && !button.long_fired) board_input_sink_(BoardInputAction::kVolumeUp);
                    if (index == 3 && !button.long_fired) board_input_sink_(BoardInputAction::kVolumeDown);
                }
                button.pressed_at_us = 0;
            }
            button.previous_pressed = pressed;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
#endif
}

voicelife::voice::AudioInputPort& VoiceLifePcbAssembly::audio_input() { return audio_ports_.input(); }
voicelife::voice::AudioOutputPort& VoiceLifePcbAssembly::audio_output() { return audio_ports_.output(); }
void VoiceLifePcbAssembly::SetOutputVolume(uint8_t volume) { audio_ports_.SetOutputVolume(volume); }
voicelife::voice::WakeGateAudioInput& VoiceLifePcbAssembly::wake_gate() { return *wake_gate_; }

void VoiceLifePcbAssembly::InitializeBoardLeds() {
#ifdef ESP_PLATFORM
    // PCB 板型专属：WS2812 上电 clear 并锁定 GPIO48 低电平。
    led_strip_config_t strip_config = {};
    strip_config.strip_gpio_num = GPIO_NUM_48;
    strip_config.max_leds = 1;
    strip_config.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;
    strip_config.led_model = LED_MODEL_WS2812;
    led_strip_rmt_config_t rmt_config = {};
    rmt_config.resolution_hz = 10 * 1000 * 1000;
    led_strip_handle_t strip = nullptr;
    if (led_strip_new_rmt_device(&strip_config, &rmt_config, &strip) == ESP_OK) {
        (void)led_strip_clear(strip);
        (void)led_strip_del(strip);
        ESP_LOGI(kPowerTag, "BUILTIN_LED_GPIO48_CLEAR=1");
    } else {
        ESP_LOGW(kPowerTag, "BUILTIN_LED_GPIO48_INIT_FAILED");
    }
    gpio_config_t led_lock = {};
    led_lock.pin_bit_mask = 1ULL << GPIO_NUM_48;
    led_lock.mode = GPIO_MODE_OUTPUT;
    led_lock.pull_up_en = GPIO_PULLUP_DISABLE;
    led_lock.pull_down_en = GPIO_PULLDOWN_ENABLE;
    led_lock.intr_type = GPIO_INTR_DISABLE;
    if (gpio_config(&led_lock) == ESP_OK) {
        (void)gpio_set_level(GPIO_NUM_48, 0);
    }
#endif
}

void VoiceLifePcbAssembly::LogAudioStats() {
#ifdef ESP_PLATFORM
    const auto stats = audio_ports_.stats();
    ESP_LOGI(
        "voicelife_pcb_audio",
        "AUDIO_STATS in_frames=%llu in_bytes=%llu in_samples=%llu in_peak=%u in_energy=%llu in_zero=%llu "
        "out_frames=%llu out_bytes=%llu out_samples=%llu out_peak=%u out_energy=%llu out_zero=%llu "
        "in_drop=%llu in_q_hi=%llu in_pool_hi=%llu in_pool_fail=%llu out_reject=%llu out_q_hi=%llu resampled=%llu "
        "min_heap=%llu "
        "volume=%u clipped=%llu short_read=%llu short_write=%llu in_i2s_err=%llu out_i2s_err=%llu "
        "test_in_frames=%llu test_in_bytes=%llu",
        static_cast<unsigned long long>(stats.captured_frames), static_cast<unsigned long long>(stats.input_pcm_bytes),
        static_cast<unsigned long long>(stats.input_samples), static_cast<unsigned>(stats.input_peak),
        static_cast<unsigned long long>(stats.input_sum_squares),
        static_cast<unsigned long long>(stats.input_zero_periods), static_cast<unsigned long long>(stats.played_frames),
        static_cast<unsigned long long>(stats.output_pcm_bytes), static_cast<unsigned long long>(stats.output_samples),
        static_cast<unsigned>(stats.output_peak), static_cast<unsigned long long>(stats.output_sum_squares),
        static_cast<unsigned long long>(stats.output_zero_periods),
        static_cast<unsigned long long>(stats.dropped_input_frames),
        static_cast<unsigned long long>(stats.input_high_watermark),
        static_cast<unsigned long long>(stats.input_payload_pool_high_watermark),
        static_cast<unsigned long long>(stats.input_payload_pool_acquisition_failures),
        static_cast<unsigned long long>(stats.rejected_output_frames),
        static_cast<unsigned long long>(stats.output_high_watermark),
        static_cast<unsigned long long>(stats.resampled_frames),
        static_cast<unsigned long long>(stats.minimum_free_heap_bytes), static_cast<unsigned>(stats.output_volume),
        static_cast<unsigned long long>(stats.output_clipped_samples),
        static_cast<unsigned long long>(stats.short_reads), static_cast<unsigned long long>(stats.short_writes),
        static_cast<unsigned long long>(stats.input_i2s_errors),
        static_cast<unsigned long long>(stats.output_i2s_errors),
        static_cast<unsigned long long>(stats.test_injected_input_frames),
        static_cast<unsigned long long>(stats.test_injected_input_bytes));
#endif
}

SparkBotAssembly::SparkBotAssembly()
    : audio_ports_(audio_esp::SparkBotEsp32s3AudioProfile(),
                   {.output_queue_depth = audio_esp::kSparkBotPlaybackQueueDepth,
                    .maximum_playback_latency_ms = audio_esp::kSparkBotPlaybackLatencyBudgetMs},
                   [this](bool enabled) { (void)SetAudioOutputEnabled(enabled); }),
      arbiter_(voicelife::board_esp::SparkBotProfile().shared_power),
      adapter_(MakeSparkBotLcdConfig(), [this](bool enabled) { ApplyBacklight(enabled); }) {}

voicelife::voice::PresentationPort& SparkBotAssembly::presentation() { return adapter_; }

void SparkBotAssembly::BoardInputTaskEntry(void* context) { static_cast<SparkBotAssembly*>(context)->BoardInputTask(); }

voicelife::Status SparkBotAssembly::StartBoardInput(BoardInputSink sink) {
    board_input_sink_ = std::move(sink);
#ifdef ESP_PLATFORM
    // 官方 SparkBot 只将 BOOT GPIO0 作为用户输入；SPI/I2S 复用引脚不参与配置。
    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << 0,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&config) != ESP_OK) {
        return Status::Error(ErrorCode::kUnavailable, "初始化 SparkBot BOOT GPIO 失败");
    }
    boot_button_ = {.gpio = 0};
    // 音频与 MultiNet 启动后内部 RAM 的最大连续块可能不足 12KB；按钮任务
    // 不执行 DMA/ISR，栈放入 PSRAM，TCB 仍由 IDF 保证位于内部 RAM。
    if (xTaskCreateWithCaps(&SparkBotAssembly::BoardInputTaskEntry, "sparkbot_button", 3072, this, 5, nullptr,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        return Status::Error(ErrorCode::kInternal, "创建 SparkBot BOOT 按键任务失败");
    }
#endif
    return Status::Ok();
}

void SparkBotAssembly::BoardInputTask() {
#ifdef ESP_PLATFORM
    constexpr int64_t kLongPressUs = 2 * 1000 * 1000;
    while (true) {
        const int64_t now = esp_timer_get_time();
        const bool pressed = gpio_get_level(static_cast<gpio_num_t>(boot_button_.gpio)) == 0;
        if (pressed && !boot_button_.previous_pressed) {
            boot_button_.pressed_at_us = now;
            boot_button_.long_fired = false;
        } else if (pressed && !boot_button_.long_fired && now - boot_button_.pressed_at_us >= kLongPressUs) {
            boot_button_.long_fired = true;
            if (board_input_sink_) board_input_sink_(BoardInputAction::kStartWifiProvisioning);
        } else if (!pressed && boot_button_.previous_pressed && board_input_sink_ && !boot_button_.long_fired) {
            board_input_sink_(BoardInputAction::kToggleChat);
        }
        if (!pressed) boot_button_.pressed_at_us = 0;
        boot_button_.previous_pressed = pressed;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
#endif
}

voicelife::voice::AudioInputPort& SparkBotAssembly::audio_input() { return audio_ports_.input(); }
voicelife::voice::AudioOutputPort& SparkBotAssembly::audio_output() { return audio_ports_.output(); }
void SparkBotAssembly::SetOutputVolume(uint8_t volume) { audio_ports_.SetOutputVolume(volume); }
voicelife::voice::WakeGateAudioInput& SparkBotAssembly::wake_gate() { return *wake_gate_; }

void SparkBotAssembly::LogAudioStats() {
#ifdef ESP_PLATFORM
    const auto stats = audio_ports_.stats();
    ESP_LOGI(
        kPowerTag,
        "AUDIO_STATS in_frames=%llu in_bytes=%llu in_samples=%llu in_peak=%u in_energy=%llu in_zero=%llu "
        "out_frames=%llu out_bytes=%llu out_samples=%llu out_peak=%u out_energy=%llu out_zero=%llu "
        "in_drop=%llu in_q_hi=%llu in_pool_hi=%llu in_pool_fail=%llu out_reject=%llu out_q_hi=%llu out_q_cap=%u "
        "out_latency_budget_ms=%u "
        "resampled=%llu min_heap=%llu "
        "volume=%u clipped=%llu short_read=%llu short_write=%llu in_i2s_err=%llu out_i2s_err=%llu "
        "test_in_frames=%llu test_in_bytes=%llu",
        static_cast<unsigned long long>(stats.captured_frames), static_cast<unsigned long long>(stats.input_pcm_bytes),
        static_cast<unsigned long long>(stats.input_samples), static_cast<unsigned>(stats.input_peak),
        static_cast<unsigned long long>(stats.input_sum_squares),
        static_cast<unsigned long long>(stats.input_zero_periods), static_cast<unsigned long long>(stats.played_frames),
        static_cast<unsigned long long>(stats.output_pcm_bytes), static_cast<unsigned long long>(stats.output_samples),
        static_cast<unsigned>(stats.output_peak), static_cast<unsigned long long>(stats.output_sum_squares),
        static_cast<unsigned long long>(stats.output_zero_periods),
        static_cast<unsigned long long>(stats.dropped_input_frames),
        static_cast<unsigned long long>(stats.input_high_watermark),
        static_cast<unsigned long long>(stats.input_payload_pool_high_watermark),
        static_cast<unsigned long long>(stats.input_payload_pool_acquisition_failures),
        static_cast<unsigned long long>(stats.rejected_output_frames),
        static_cast<unsigned long long>(stats.output_high_watermark),
        static_cast<unsigned>(audio_esp::kSparkBotPlaybackQueueDepth),
        static_cast<unsigned>(audio_esp::kSparkBotPlaybackLatencyBudgetMs),
        static_cast<unsigned long long>(stats.resampled_frames),
        static_cast<unsigned long long>(stats.minimum_free_heap_bytes), static_cast<unsigned>(stats.output_volume),
        static_cast<unsigned long long>(stats.output_clipped_samples),
        static_cast<unsigned long long>(stats.short_reads), static_cast<unsigned long long>(stats.short_writes),
        static_cast<unsigned long long>(stats.input_i2s_errors),
        static_cast<unsigned long long>(stats.output_i2s_errors),
        static_cast<unsigned long long>(stats.test_injected_input_frames),
        static_cast<unsigned long long>(stats.test_injected_input_bytes));
#endif
}

voicelife::Status SparkBotAssembly::Start() {
    ConfigureSharedPowerGpio();
    // 显示启动：经统一仲裁启用背光。
    ApplyBacklight(true);
    const Status display = adapter_.Start();
    if (!display.ok()) return display;
    // Keep the board profiles equal at the voice boundary: both boards use
    // the existing MultiNet7 command grammar for "你好牛牛". ESP-SR emits the
    // model partition image from the selected profile at build time.
    wake_detector_ = std::make_unique<audio_esp::EspMultiNetWakeDetector>();
    wake_gate_ = std::make_unique<voice::WakeGateAudioInput>(audio_ports_.input(), *wake_detector_, true);
    wake_ready_ = true;
#ifdef ESP_PLATFORM
    ESP_LOGI(kPowerTag, "SPARKBOT_ASSEMBLY_READY display=1 wake=1");
#endif
    return Status::Ok();
}

voicelife::Status SparkBotAssembly::SetAudioOutputEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(power_mutex_);
#ifdef ESP_PLATFORM
    ESP_LOGI(kPowerTag, "GPIO46_AUDIO_REQUEST=%d", enabled ? 1 : 0);
#endif
    (void)arbiter_.SetAudioOutputEnabled(enabled);
    WriteSharedPowerLineLocked();
    return voicelife::Status::Ok();
}

void SparkBotAssembly::ApplyBacklight(bool enabled) {
    std::lock_guard<std::mutex> lock(power_mutex_);
#ifdef ESP_PLATFORM
    ESP_LOGI(kPowerTag, "GPIO46_BACKLIGHT_REQUEST=%d", enabled ? 1 : 0);
#endif
    (void)arbiter_.SetBacklightEnabled(enabled);
    WriteSharedPowerLineLocked();
}

void SparkBotAssembly::ConfigureSharedPowerGpio() {
#ifdef ESP_PLATFORM
    const auto profile = voicelife::board_esp::SparkBotProfile().shared_power;
    if (profile.gpio < 0) {
        return;
    }
    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << profile.gpio,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    (void)gpio_config(&config);
#else
    (void)0;
#endif
}

void SparkBotAssembly::WriteSharedPowerLineLocked() {
#ifdef ESP_PLATFORM
    const auto profile = voicelife::board_esp::SparkBotProfile().shared_power;
    if (profile.gpio < 0) {
        return;
    }
    const int level = arbiter_.line_enabled() ? (profile.active_high ? 1 : 0) : (profile.active_high ? 0 : 1);
    (void)gpio_set_level(static_cast<gpio_num_t>(profile.gpio), level);
#else
    (void)0;
#endif
}

}  // namespace voicelife::runtime
