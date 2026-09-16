file(GLOB DOOMGENERIC_SOURCES "${CMAKE_CURRENT_LIST_DIR}/*.c")

# DoomGeneric ships several desktop backends.  The ESP32 platform callbacks
# live in main/emulator/emulator_doom.c, so only retain the shared engine.
list(FILTER DOOMGENERIC_SOURCES EXCLUDE REGEX ".*/(doomgeneric_(allegro|emscripten|linuxvt|sdl|soso|sosox|win|xlib)|i_allegromusic|i_allegrosound|i_sdlmusic|i_sdlsound|icon|gusconf|mus2mid)\\.c$")

idf_component_register(
    SRCS ${DOOMGENERIC_SOURCES}
    INCLUDE_DIRS "."
    REQUIRES esp_timer
)

# Doom itself renders at 320x200.  Keeping that native framebuffer avoids
# modifying the game renderer; the ESP32 display adapter scales it to 240x150.
target_compile_definitions(${COMPONENT_LIB} PUBLIC DOOMGENERIC_RESX=320 DOOMGENERIC_RESY=200)
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error
    -Wno-error=implicit-fallthrough
    -Wno-error=missing-field-initializers
    -Wno-error=unterminated-string-initialization
)
