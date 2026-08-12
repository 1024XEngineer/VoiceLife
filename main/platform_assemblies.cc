#include "platform_assemblies.h"

#include "voicelife/board_esp/sparkbot_profile.h"
#include "voicelife/display_sparkbot/sparkbot_lvgl_display.h"

#ifdef ESP_PLATFORM
#include <driver/gpio.h>
#include <esp_log.h>
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

VoiceLifePcbAssembly::VoiceLifePcbAssembly() : audio_ports_(audio_esp::VoiceLifePcbEsp32s3Profile()) {}

voicelife::voice::PresentationPort& VoiceLifePcbAssembly::presentation() { return ssd1306_adapter_; }

audio_esp::AudioBoardProfile VoiceLifePcbAssembly::audio_profile() const {
    return audio_esp::VoiceLifePcbEsp32s3Profile();
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
    ESP_LOGI("voicelife_pcb_audio", "AUDIO_STATS input=%llu output=%llu short_write=%llu",
             static_cast<unsigned long long>(stats.captured_frames),
             static_cast<unsigned long long>(stats.played_frames), static_cast<unsigned long long>(stats.short_writes));
#endif
}

SparkBotAssembly::SparkBotAssembly()
    : audio_ports_(audio_esp::SparkBotEsp32s3AudioProfile(), {},
                   [this](bool enabled) { (void)SetAudioOutputEnabled(enabled); }),
      arbiter_(voicelife::board_esp::SparkBotProfile().shared_power),
      adapter_(MakeSparkBotLcdConfig(), [this](bool enabled) { ApplyBacklight(enabled); }) {
    wake_detector_ = std::make_unique<audio_esp::EspMultiNetWakeDetector>();
    wake_gate_ = std::make_unique<voice::WakeGateAudioInput>(audio_ports_.input(), *wake_detector_);
}

voicelife::voice::PresentationPort& SparkBotAssembly::presentation() { return adapter_; }

audio_esp::AudioBoardProfile SparkBotAssembly::audio_profile() const {
    return audio_esp::SparkBotEsp32s3AudioProfile();
}

voicelife::voice::AudioInputPort& SparkBotAssembly::audio_input() { return audio_ports_.input(); }
voicelife::voice::AudioOutputPort& SparkBotAssembly::audio_output() { return audio_ports_.output(); }
void SparkBotAssembly::SetOutputVolume(uint8_t volume) { audio_ports_.SetOutputVolume(volume); }
voicelife::voice::WakeGateAudioInput& SparkBotAssembly::wake_gate() { return *wake_gate_; }

void SparkBotAssembly::LogAudioStats() {
#ifdef ESP_PLATFORM
    const auto stats = audio_ports_.stats();
    ESP_LOGI(kPowerTag, "AUDIO_STATS input=%llu output=%llu short_write=%llu",
             static_cast<unsigned long long>(stats.captured_frames),
             static_cast<unsigned long long>(stats.played_frames), static_cast<unsigned long long>(stats.short_writes));
#endif
}

voicelife::Status SparkBotAssembly::Start() {
    ConfigureSharedPowerGpio();
    // 显示启动：经统一仲裁启用背光。
    ApplyBacklight(true);
    return adapter_.Start();
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
