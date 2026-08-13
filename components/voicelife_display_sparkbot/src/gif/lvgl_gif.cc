#include "lvgl_gif.h"

#include <esp_log.h>

#include <cstring>

#define TAG "LvglGif"

namespace voicelife::display_sparkbot {

LvglGif::LvglGif(const lv_img_dsc_t* img_dsc)
    : LvglGif(img_dsc ? img_dsc->data : nullptr, img_dsc ? img_dsc->data_size : 0) {}

LvglGif::LvglGif(const uint8_t* data, std::size_t size)
    : gif_(nullptr),
      timer_(nullptr),
      last_call_(0),
      playing_(false),
      loaded_(false),
      loop_delay_ms_(0),
      loop_waiting_(false),
      loop_wait_start_(0) {
    if (!data || size == 0) {
        ESP_LOGE(TAG, "Invalid GIF data view");
        return;
    }

    gif_ = gd_open_gif_data_sized(data, size);
    if (!gif_) {
        ESP_LOGE(TAG, "Failed to open GIF from image descriptor");
        return;
    }

    // Setup LVGL image descriptor
    memset(&img_dsc_, 0, sizeof(img_dsc_));
    img_dsc_.header.magic = LV_IMAGE_HEADER_MAGIC;
    img_dsc_.header.flags = LV_IMAGE_FLAGS_MODIFIABLE;
    img_dsc_.header.cf = LV_COLOR_FORMAT_ARGB8888;
    img_dsc_.header.w = gif_->width;
    img_dsc_.header.h = gif_->height;
    img_dsc_.header.stride = gif_->width * 4;
    img_dsc_.data = gif_->canvas;
    img_dsc_.data_size = gif_->width * gif_->height * 4;

    loaded_ = true;
    ESP_LOGD(TAG, "GIF loaded from image descriptor: %dx%d", gif_->width, gif_->height);
}

// Destructor
LvglGif::~LvglGif() { Cleanup(); }

// LvglImage interface implementation
const lv_img_dsc_t* LvglGif::image_dsc() const {
    if (!loaded_) {
        return nullptr;
    }
    return &img_dsc_;
}

// Animation control methods
bool LvglGif::Start() {
    if (!loaded_ || !gif_) {
        ESP_LOGW(TAG, "GIF not loaded, cannot start");
        return false;
    }

    if (!timer_) {
        timer_ = lv_timer_create(
            [](lv_timer_t* timer) {
                LvglGif* gif_obj = static_cast<LvglGif*>(lv_timer_get_user_data(timer));
                gif_obj->NextFrame();
            },
            10, this);
    }

    if (timer_) {
        playing_ = true;
        frame_count_ = 0;
        loop_count_observed_ = 0;
        loop_waiting_ = false;  // Reset loop waiting state
        last_call_ = lv_tick_get();
        lv_timer_resume(timer_);
        lv_timer_reset(timer_);

        // Decode and render the first frame. Rendering before gd_get_frame()
        // draws an uninitialized frame rectangle for optimized GIFs.
        NextFrame();
        if (!playing_) {
            // NextFrame failed and paused the timer. Do not let callers hide
            // the fallback glyph behind an undecoded image descriptor.
            return false;
        }
        ESP_LOGI(TAG, "SPARKBOT_GIF_FIRST_FRAME_OK width=%u height=%u", static_cast<unsigned>(gif_->width),
                 static_cast<unsigned>(gif_->height));
        return true;
    }
    return false;
}

void LvglGif::Pause() {
    if (timer_) {
        playing_ = false;
        lv_timer_pause(timer_);
        ESP_LOGD(TAG, "GIF animation paused");
    }
}

void LvglGif::Resume() {
    if (!loaded_ || !gif_) {
        ESP_LOGW(TAG, "GIF not loaded, cannot resume");
        return;
    }

    if (timer_) {
        playing_ = true;
        lv_timer_resume(timer_);
        ESP_LOGD(TAG, "GIF animation resumed");
    }
}

void LvglGif::Stop() {
    if (timer_) {
        playing_ = false;
        lv_timer_pause(timer_);
    }

    // Reset loop waiting state
    loop_waiting_ = false;

    if (gif_) {
        gd_rewind(gif_);
        // Render first frame without advancing
        if (gif_->canvas) {
            gd_render_frame(gif_, gif_->canvas);
        }
        ESP_LOGI(TAG, "SPARKBOT_GIF_STOPPED asset=%s frames=%u loops=%u", telemetry_asset_.c_str(),
                 static_cast<unsigned>(frame_count_), static_cast<unsigned>(loop_count_observed_));
    }
}

bool LvglGif::IsPlaying() const { return playing_; }

bool LvglGif::IsLoaded() const { return loaded_; }

int32_t LvglGif::GetLoopCount() const {
    if (!loaded_ || !gif_) {
        return -1;
    }
    return gif_->loop_count;
}

void LvglGif::SetLoopCount(int32_t count) {
    if (!loaded_ || !gif_) {
        ESP_LOGW(TAG, "GIF not loaded, cannot set loop count");
        return;
    }
    gif_->loop_count = count;
}

uint32_t LvglGif::GetLoopDelay() const { return loop_delay_ms_; }

void LvglGif::SetLoopDelay(uint32_t delay_ms) {
    loop_delay_ms_ = delay_ms;
    ESP_LOGD(TAG, "Loop delay set to %lu ms", delay_ms);
}

uint16_t LvglGif::width() const {
    if (!loaded_ || !gif_) {
        return 0;
    }
    return gif_->width;
}

uint16_t LvglGif::height() const {
    if (!loaded_ || !gif_) {
        return 0;
    }
    return gif_->height;
}

void LvglGif::SetFrameCallback(std::function<void()> callback) { frame_callback_ = callback; }

void LvglGif::SetTelemetryAsset(std::string_view asset) { telemetry_asset_ = std::string(asset); }

void LvglGif::NextFrame() {
    if (!loaded_ || !gif_ || !playing_) {
        return;
    }

    // Check if we're in loop wait state (only for infinite loop GIFs with delay)
    if (loop_waiting_) {
        uint32_t wait_elapsed = lv_tick_elaps(loop_wait_start_);
        if (wait_elapsed < loop_delay_ms_) {
            // Still waiting for loop delay
            return;
        }
        // Loop delay completed, continue playing
        loop_waiting_ = false;
        ESP_LOGD(TAG, "Loop delay completed, continuing GIF");
    }

    // Check if enough time has passed for the next frame
    uint32_t elapsed = lv_tick_elaps(last_call_);
    if (elapsed < gif_->gce.delay * 10) {
        return;
    }

    last_call_ = lv_tick_get();

    // Save file position before getting next frame to detect loop
    uint32_t pos_before = gif_->f_rw_p;

    // Get next frame
    int has_next = gd_get_frame(gif_);
    if (has_next < 0) {
        // 解码错误（越界/坏帧）：停止动画并回退，不渲染坏帧。
        ESP_LOGE(TAG, "GIF_DECODE_FAILED pos=%u", static_cast<unsigned>(pos_before));
        playing_ = false;
        if (timer_) {
            lv_timer_pause(timer_);
        }
        return;
    }
    if (has_next == 0) {
        // Animation truly finished (non-infinite loop)
        playing_ = false;
        if (timer_) {
            lv_timer_pause(timer_);
        }
        ESP_LOGD(TAG, "GIF animation completed");
        return;
    }

    // Detect loop by checking if file position jumped back (rewound to start)
    // This works for looping GIFs regardless of when loop_count is set
    if (gif_->f_rw_p < pos_before) {
        ++loop_count_observed_;
        ESP_LOGI(TAG, "SPARKBOT_GIF_LOOP asset=%s frames=%u loops=%u", telemetry_asset_.c_str(),
                 static_cast<unsigned>(frame_count_), static_cast<unsigned>(loop_count_observed_));
    }
    if (loop_delay_ms_ > 0 && gif_->f_rw_p < pos_before) {
        // File position decreased, meaning GIF looped back to beginning
        // Start waiting before rendering this frame
        loop_waiting_ = true;
        loop_wait_start_ = lv_tick_get();
        ESP_LOGD(TAG, "GIF completed one cycle, waiting %lu ms before next loop", loop_delay_ms_);
        return;
    }

    // Render current frame
    if (gif_->canvas) {
        gd_render_frame(gif_, gif_->canvas);

        ++frame_count_;
        // 约每 30 帧输出一次，证明 idle 等动画持续推进而不刷爆串口。
        if (frame_count_ % 30U == 0U) {
            ESP_LOGI(TAG, "SPARKBOT_GIF_FRAME_ADVANCE asset=%s frames=%u loops=%u", telemetry_asset_.c_str(),
                     static_cast<unsigned>(frame_count_), static_cast<unsigned>(loop_count_observed_));
        }

        // Call frame callback if set
        if (frame_callback_) {
            frame_callback_();
        }
    }
}

void LvglGif::Cleanup() {
    // Stop and delete timer
    if (timer_) {
        lv_timer_delete(timer_);
        timer_ = nullptr;
    }

    // Close GIF decoder
    if (gif_) {
        gd_close_gif(gif_);
        gif_ = nullptr;
    }

    playing_ = false;
    loaded_ = false;

    // Clear image descriptor
    memset(&img_dsc_, 0, sizeof(img_dsc_));
}

}  // namespace voicelife::display_sparkbot
