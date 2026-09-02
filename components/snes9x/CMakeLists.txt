idf_component_register(
    SRCS "snes9x.c"
         "src/cpu.c" "src/cpuexec.c" "src/cpuops.c" "src/memmap.c" "src/ppu.c"
         "src/gfx.c" "src/tile.c" "src/clip.c" "src/dma.c" "src/getset.c" "src/globals.c"
         "src/apu.c" "src/spc700.c" "src/soundux.c" "src/dsp.c"
         "src/c4.c" "src/c4emu.c" "src/obc1.c" "src/srtc.c" "src/snapshot.c"
    INCLUDE_DIRS "." "src"
    REQUIRES esp_common heap
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    RIGHTSHIFT_IS_SAR
    FAST_LSB_WORD_ACCESS
    NO_ZERO_LUT
    ZSNES_FX
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -O3
    -Wno-unused-function
    -Wno-unused-variable
    -Wno-array-bounds
    -Wno-implicit-fallthrough
    -Wno-unused-label
    -Wno-format
    -Wno-sign-compare
    -Wno-main
)
