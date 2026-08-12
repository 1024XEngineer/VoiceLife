#include "runtime_board_input.h"

#ifdef ESP_PLATFORM

#include <cstdint>
#include <memory>
#include <utility>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace voicelife::runtime {
namespace {

constexpr char kTag[] = "VoiceLifeRuntime";
constexpr gpio_num_t kBootButtonGpio = GPIO_NUM_0;
constexpr gpio_num_t kTouchButtonGpio = GPIO_NUM_47;
constexpr gpio_num_t kVolumeUpButtonGpio = GPIO_NUM_40;
constexpr gpio_num_t kVolumeDownButtonGpio = GPIO_NUM_39;
constexpr int64_t kLongPressUs = 2000000;

}  // namespace

struct VoiceLifePcbBoardInput::ButtonSample {
    gpio_num_t gpio = GPIO_NUM_NC;
    bool previous_pressed = false;
    bool long_fired = false;
    int64_t pressed_at_us = 0;
};

VoiceLifePcbBoardInput::VoiceLifePcbBoardInput(InteractionSink interaction_sink, VolumeSink volume_sink)
    : interaction_sink_(std::move(interaction_sink)),
      volume_sink_(std::move(volume_sink)),
      buttons_(std::make_unique<ButtonSample[]>(4)) {}

VoiceLifePcbBoardInput::~VoiceLifePcbBoardInput() = default;

void VoiceLifePcbBoardInput::Start() {
    const gpio_config_t config = {
        .pin_bit_mask = (1ULL << kBootButtonGpio) | (1ULL << kTouchButtonGpio) | (1ULL << kVolumeUpButtonGpio) |
                        (1ULL << kVolumeDownButtonGpio),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&config));
    buttons_[0].gpio = kBootButtonGpio;
    buttons_[1].gpio = kTouchButtonGpio;
    buttons_[2].gpio = kVolumeUpButtonGpio;
    buttons_[3].gpio = kVolumeDownButtonGpio;
    if (xTaskCreate(&TaskEntry, "voicelife_buttons", 3072, this, 5, nullptr) != pdPASS) {
        ESP_LOGW(kTag, "创建板级按键任务失败");
    }
}

void VoiceLifePcbBoardInput::TaskEntry(void* context) { static_cast<VoiceLifePcbBoardInput*>(context)->Run(); }

void VoiceLifePcbBoardInput::Run() {
    while (true) {
        const int64_t now = esp_timer_get_time();
        for (std::size_t index = 0; index < 4; ++index) {
            ButtonSample& button = buttons_[index];
            const bool pressed = gpio_get_level(button.gpio) == 0;
            if (pressed && !button.previous_pressed) {
                button.pressed_at_us = now;
                button.long_fired = false;
                ESP_LOGI(kTag, "BUTTON_EVENT gpio=%d action=down", static_cast<int>(button.gpio));
                if (index == 1 && interaction_sink_) interaction_sink_(voice::VoiceInteractionEvent::kPressDown);
            } else if (pressed && !button.long_fired && now - button.pressed_at_us >= kLongPressUs) {
                button.long_fired = true;
                ESP_LOGI(kTag, "BUTTON_EVENT gpio=%d action=long", static_cast<int>(button.gpio));
                if (volume_sink_ && index == 2) {
                    volume_sink_(100);
                } else if (volume_sink_ && index == 3) {
                    volume_sink_(0);
                }
            } else if (!pressed && button.previous_pressed) {
                ESP_LOGI(kTag, "BUTTON_EVENT gpio=%d action=up duration_ms=%lld", static_cast<int>(button.gpio),
                         static_cast<long long>((now - button.pressed_at_us) / 1000));
                if (index == 0 && !button.long_fired && interaction_sink_) {
                    interaction_sink_(voice::VoiceInteractionEvent::kToggleChat);
                } else if (index == 1 && interaction_sink_) {
                    interaction_sink_(voice::VoiceInteractionEvent::kPressUp);
                } else if (index == 2 && !button.long_fired && volume_sink_) {
                    volume_sink_(10);
                } else if (index == 3 && !button.long_fired && volume_sink_) {
                    volume_sink_(-10);
                }
                button.pressed_at_us = 0;
            }
            button.previous_pressed = pressed;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

}  // namespace voicelife::runtime

#endif
