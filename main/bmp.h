#pragma once

#include <stdint.h>

/* Decode a 24/32-bit uncompressed BMP file into an RGB565 buffer, scaling to
 * out_w x out_h with nearest-neighbor sampling. Returns 0 on success, -1 if
 * the file is missing, corrupt, or uses an unsupported format. */
int bmp_decode(const char *path, uint16_t *out, int out_w, int out_h);
