idf_component_register(
    SRC_DIRS "."
    INCLUDE_DIRS "."
    REQUIRES esp_common heap
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE SMS_INTERNAL)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -O3
    -Wno-unused-function
    -Wno-unused-variable
)
