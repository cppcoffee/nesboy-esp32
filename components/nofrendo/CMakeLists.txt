idf_component_register(
    SRC_DIRS "." "nes" "mappers"
    INCLUDE_DIRS "."
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -O3
    -Wno-array-bounds
    -Wno-format
    -Wno-unused-variable
    -Wno-unused-function
)
