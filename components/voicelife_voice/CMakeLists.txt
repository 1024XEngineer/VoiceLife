idf_component_register(
    SRCS
        "src/voice_provider_registry.cc"
        "src/voice_session.cc"
        "src/voice_session_coordinator.cc"
    INCLUDE_DIRS "include"
    REQUIRES voicelife_contracts
)
