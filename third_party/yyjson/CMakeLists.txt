idf_component_register(
    SRCS "yyjson.c"
    INCLUDE_DIRS "."
)

target_compile_options(${COMPONENT_LIB} PRIVATE -w)
