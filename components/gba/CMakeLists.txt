idf_component_register(
    SRCS "gba.c" "main.c" "gba_memory.c" "gba_cc_lut.c" "cpu.cpp" "video.cpp" "sound.c" "input.c" "savestate.c" "cheats.c" "serial.c" "gbp.c" "rfu.c"
    INCLUDE_DIRS "."
    REQUIRES esp_common heap
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    ROM_BUFFER_SIZE=4
    OVERCLOCK_60FPS
    GBSP_PSRAM
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -O3
    -fomit-frame-pointer
    -Wno-unused-variable
    -Wno-unused-function
    -Wno-implicit-fallthrough
    -Wno-missing-field-initializers
    -Wno-unused-label
    -Wno-format
    -Wno-sign-compare
)

# The interpreter is a huge switch: keep the hot text out of flash cache
# pressure and let GCC do the heavy lifting.
target_link_options(${COMPONENT_LIB} PRIVATE "")
