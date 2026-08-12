#pragma once

/* SD card file browser / ROM picker.
 *
 * Run a full-screen menu (uses ui_*) that lets the user navigate the SD card
 * directory tree and select a .nes, .gb, or .gbc file. On success the chosen absolute path
 * is written to out_path. Returns 0 on selection, -1 if the user cancelled
 * (pressed B at the root) or the SD card is not usable. */
int browser_run(char *out_path, int out_path_size);
