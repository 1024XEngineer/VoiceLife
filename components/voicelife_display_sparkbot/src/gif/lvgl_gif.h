#pragma once

#include <lvgl.h>

#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "gifdec.h"

/**
 * C++ implementation of LVGL GIF widget
 * Provides GIF animation functionality using gifdec library
 */
namespace voicelife::display_sparkbot {

class LvglGif {
   public:
    explicit LvglGif(const lv_img_dsc_t* img_dsc);

    /**
     * @brief 按资源视图构造（GIF 数据 + 大小，所有权仍归调用方）。
     * @param data GIF 字节流（assets mmap 只读视图）。
     * @param size 字节数。
     */
    LvglGif(const uint8_t* data, std::size_t size);
    virtual ~LvglGif();

    // LvglImage interface implementation
    virtual const lv_img_dsc_t* image_dsc() const;

    /**
     * Start/restart GIF animation
     */
    /** @return true only after the first frame has been decoded and rendered. */
    bool Start();

    /**
     * Pause GIF animation
     */
    void Pause();

    /**
     * Resume GIF animation
     */
    void Resume();

    /**
     * Stop GIF animation and rewind to first frame
     */
    void Stop();

    /**
     * Check if GIF is currently playing
     */
    bool IsPlaying() const;

    /**
     * Check if GIF was loaded successfully
     */
    bool IsLoaded() const;

    /**
     * Get loop count
     */
    int32_t GetLoopCount() const;

    /**
     * Set loop count
     */
    void SetLoopCount(int32_t count);

    /**
     * Get loop delay in milliseconds (delay between loops)
     */
    uint32_t GetLoopDelay() const;

    /**
     * Set loop delay in milliseconds (delay between loops)
     * @param delay_ms Delay in milliseconds before starting next loop. 0 means no delay.
     */
    void SetLoopDelay(uint32_t delay_ms);

    /**
     * Get GIF dimensions
     */
    uint16_t width() const;
    uint16_t height() const;

    /**
     * Set frame update callback
     */
    void SetFrameCallback(std::function<void()> callback);

    /** 设置仅用于低频运行证据的受控 emotion key。 */
    void SetTelemetryAsset(std::string_view asset);

   private:
    // GIF decoder instance
    gd_GIF* gif_;

    // LVGL image descriptor
    lv_img_dsc_t img_dsc_;

    // Animation timer
    lv_timer_t* timer_;

    // Last frame update time
    uint32_t last_call_;

    // Animation state
    bool playing_;
    bool loaded_;

    // Loop delay configuration
    uint32_t loop_delay_ms_;    // Delay between loops in milliseconds
    bool loop_waiting_;         // Whether we're waiting for the next loop
    uint32_t loop_wait_start_;  // Timestamp when loop wait started

    // Frame update callback
    std::function<void()> frame_callback_;
    std::string telemetry_asset_;
    uint32_t frame_count_ = 0;
    uint32_t loop_count_observed_ = 0;

    /**
     * Update to next frame
     */
    void NextFrame();

    /**
     * Cleanup resources
     */
    void Cleanup();
};

}  // namespace voicelife::display_sparkbot
