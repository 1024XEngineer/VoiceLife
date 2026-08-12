idf_component_register(
    SRCS "src/sparkbot_lvgl_display.cc" "src/sparkbot_lvgl_renderer.cc"
    INCLUDE_DIRS "include"
    REQUIRES voicelife_contracts voicelife_voice
)
