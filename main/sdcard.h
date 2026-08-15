#ifndef SDCARD_H
#define SDCARD_H

#include <stdbool.h>
#include <stddef.h>

/*
 * SD card (SPI mode) on SPI3_HOST.
 *
 * Initializes the SPI3 bus and mounts the FAT filesystem at "/sdcard".
 * The card stays mounted afterwards (for ROM loading / saves).
 * Returns true on success. On failure returns false and, if err_out is
 * non-NULL, writes a short description (the ESP-IDF error name) into it.
 */
bool sdcard_mount(char *err_out, size_t err_out_len);

#endif // SDCARD_H
