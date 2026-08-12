#ifndef SDCARD_H
#define SDCARD_H

#include <stdbool.h>

/*
 * SD card (SPI mode) on SPI3_HOST.
 *
 * Initializes the SPI3 bus and mounts the FAT filesystem at "/sdcard".
 * The card stays mounted afterwards (for ROM loading / saves).
 * Returns true on success, false on failure (logged under "sdcard").
 */
bool sdcard_mount(void);

#endif // SDCARD_H
