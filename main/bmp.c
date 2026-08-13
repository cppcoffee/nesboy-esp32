#include "bmp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Largest source image accepted (2 MB covers ~512x512 32bpp). */
#define BMP_MAX_FILE (2 * 1024 * 1024)

#define LE16(p) ((uint16_t)((p)[0]) | ((uint16_t)((p)[1]) << 8))
#define LE32(p)                                                                                                        \
    ((uint32_t)((p)[0]) | ((uint32_t)((p)[1]) << 8) | ((uint32_t)((p)[2]) << 16) | ((uint32_t)((p)[3]) << 24))

int bmp_decode(const char *path, uint16_t *out, int out_w, int out_h)
{
    if (!path || !out || out_w <= 0 || out_h <= 0) {
        return -1;
    }

    FILE *file = fopen(path, "rb");
    if (!file) {
        return -1;
    }
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (size < 54 || size > BMP_MAX_FILE) {
        fclose(file);
        return -1;
    }

    uint8_t *data = malloc((size_t)size);
    if (!data) {
        fclose(file);
        return -1;
    }
    if (fread(data, 1, (size_t)size, file) != (size_t)size) {
        free(data);
        fclose(file);
        return -1;
    }
    fclose(file);

    if (memcmp(data, "BM", 2) != 0 || LE32(data + 14) < 40) {
        free(data);
        return -1;
    }

    int32_t width = (int32_t)LE32(data + 18);
    int32_t height = (int32_t)LE32(data + 22);
    uint16_t bpp = LE16(data + 28);
    uint32_t compression = LE32(data + 30);
    uint32_t data_offset = LE32(data + 10);
    if (width <= 0 || height == 0 || (bpp != 24 && bpp != 32) || compression != 0 || data_offset >= (uint32_t)size) {
        free(data);
        return -1;
    }

    int bottom_up = height > 0;
    int src_h = bottom_up ? height : -height;
    int bytes = bpp / 8;
    int stride = ((width * bytes) + 3) & ~3;
    if ((long)data_offset + (long)stride * src_h > size) {
        free(data);
        return -1;
    }
    const uint8_t *pixels = data + data_offset;

    for (int dy = 0; dy < out_h; dy++) {
        int sy = src_h * dy / out_h;
        if (sy >= src_h) {
            sy = src_h - 1;
        }
        const uint8_t *row = pixels + (size_t)(bottom_up ? (src_h - 1 - sy) : sy) * stride;
        for (int dx = 0; dx < out_w; dx++) {
            int sx = width * dx / out_w;
            const uint8_t *p = row + (size_t)sx * bytes;
            uint16_t rgb = (uint16_t)(((p[2] >> 3) << 11) | ((p[1] >> 2) << 5) | (p[0] >> 3));
            out[(size_t)dy * out_w + dx] = rgb;
        }
    }

    free(data);
    return 0;
}
