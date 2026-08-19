idf_component_register(
    SRCS
        "src/audio_frame_queue.cc"
        "src/audio_payload.cc"
        "src/audio_aec.cc"
        "src/wake_gate_audio_input.cc"
        "src/voice_interaction_controller.cc"
        "src/voice_provider_registry.cc"
        "src/voice_session.cc"
        "src/voice_session_coordinator.cc"
    INCLUDE_DIRS "include"
    REQUIRES voicelife_contracts
)
