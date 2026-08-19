idf_component_register(
    SRCS "src/im_reminder_bridge.cc"
    INCLUDE_DIRS "include"
    REQUIRES voicelife_schedule voicelife_im voicelife_contracts
)
