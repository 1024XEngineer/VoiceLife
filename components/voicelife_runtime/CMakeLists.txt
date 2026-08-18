idf_component_register(
    SRCS "src/runtime.cc"
    INCLUDE_DIRS "include"
    REQUIRES voicelife_contracts
    PRIV_REQUIRES voicelife_runtime_esp
)
