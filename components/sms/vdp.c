/* SMS VDP mode 4 (the graphics mode used by virtually all commercial games).
 *
 * Timing: 262 scanlines x 228 t-cycles per frame (NTSC), 192 visible lines.
 * The Z80 is driven in lockstep from sms_run_frame; the VDP consumes its
 * share of cycles per scanline and renders each line as it finishes.
 */
#include <string.h>

#include "sms_internal.h"
#include "vdp.h"

#define LINES_PER_FRAME 262
#define CYCLES_PER_LINE 228
#define VISIBLE_LINES   192

/* register indices */
#define VRAM_SIZE 0x4000
#define CRAM_SIZE 32
#define SAT_SIZE  64

struct vdp_state {
    uint8_t vram[VRAM_SIZE];
    uint8_t cram[CRAM_SIZE];
    uint8_t sat[SAT_SIZE]; /* sprite attribute table shadow */

    uint8_t regs[16];
    uint16_t addr;
    uint8_t code;
    bool first_write;
    bool control_byte;

    int line;
    int line_cycles;
    int status_flags;

    /* line buffer: palette indices, then converted to RGB565 at blit time */
    uint8_t linebuf[SMS_WIDTH];

    /* pending interrupt lines */
    bool irq_vblank;
    bool irq_line;

    uint16_t rgb_palette[32];
};

static struct vdp_state vd;

static inline int screen_width(void)
{
    return sms.hw == SMS_HW_GG ? GG_WIDTH : SMS_WIDTH;
}

static inline int screen_height(void)
{
    return sms.hw == SMS_HW_GG ? GG_HEIGHT : VISIBLE_LINES;
}

static void cram_update(void)
{
    for (int i = 0; i < 32; i++) {
        uint8_t v = vd.cram[i];
        int r = (v >> 0) & 3;
        int g = (v >> 2) & 3;
        int b = (v >> 4) & 3;
        /* expand 2-bit channels to 5-bit with replication */
        static const uint8_t expand[4] = {0, 8 + 4, 16 + 8 + 4 + 2, 31};
        vd.rgb_palette[i] = (uint16_t)((expand[r] << 11) | (expand[g] << 6) | expand[b]);
    }
}

void vdp_reset(void)
{
    memset(&vd, 0, sizeof(vd));
    vd.first_write = true;
    vd.regs[0] = 0x36;
    vd.regs[1] = 0x80;
    vd.regs[10] = 0xFF;
    cram_update();
}

uint8_t vdp_vcounter(void)
{
    int l = vd.line;
    if (l > 0xDA && l < 0xE5) {
        l -= 6; /* NTSC counter jump */
    }
    return (uint8_t)l;
}

uint8_t vdp_read(uint8_t port)
{
    if (!(port & 1)) {
        /* data port */
        uint8_t v = vd.vram[vd.addr & (VRAM_SIZE - 1)];
        vd.addr = (vd.addr + 1) & 0x3FFF;
        vd.first_write = true;
        return v;
    }

    /* control/status port */
    uint8_t v = (uint8_t)(vd.status_flags | 0x1F);
    vd.status_flags = 0;
    vd.irq_vblank = false;
    vd.first_write = true;
    return v;
}

void vdp_write(uint8_t port, uint8_t value)
{
    if (!(port & 1)) {
        /* data port */
        switch (vd.code) {
        case 0:
        case 1:
        case 2:
            vd.vram[vd.addr & (VRAM_SIZE - 1)] = value;
            break;
        case 3:
            vd.cram[vd.addr & (CRAM_SIZE - 1)] = value;
            cram_update();
            break;
        default:
            break;
        }
        vd.addr = (vd.addr + 1) & 0x3FFF;
        vd.first_write = true;
        return;
    }

    /* control port */
    if (vd.first_write) {
        vd.addr = (uint16_t)((vd.addr & 0x3F00) | value);
        vd.control_byte = true;
        vd.first_write = false;
        return;
    }

    vd.first_write = true;
    uint8_t cmd = value >> 6;
    vd.addr = (uint16_t)(((value & 0x3F) << 8) | (vd.addr & 0xFF));
    vd.code = (uint8_t)(((value >> 4) & 0x03) << 2); /* unused beyond cmd */

    switch (cmd) {
    case 0: /* VRAM read */
        vd.code = 0;
        break;
    case 1: /* VRAM write */
        vd.code = 1;
        break;
    case 2: /* register write */
    {
        uint8_t reg = (uint8_t)((value >> 0) & 0x0F);
        uint8_t val = (uint8_t)(vd.addr & 0xFF);
        if (reg < 16) {
            vd.regs[reg] = val;
        }
        break;
    }
    case 3: /* CRAM write */
        vd.code = 3;
        break;
    }
}

/* ---- rendering ---- */

static void render_line(int ly)
{
    if (!sms.video_cb || !sms.framebuffer) {
        return;
    }
    int w = screen_width();
    int h = screen_height();

    int y = ly - (sms.hw == SMS_HW_GG ? 48 : 0);
    int xoff = sms.hw == SMS_HW_GG ? 48 : 0;
    if (y < 0 || y >= h) {
        return;
    }
    (void)xoff;

    bool display_on = (vd.regs[1] & 0x40) != 0;
    uint8_t bg_color = (uint8_t)(vd.regs[7] & 0x0F);

    if (!display_on) {
        uint16_t *dst = sms.framebuffer + (size_t)y * w;
        uint16_t c = vd.rgb_palette[bg_color & 0x0F];
        for (int x = 0; x < w; x++) {
            dst[x] = c;
        }
        return;
    }

    uint8_t *lb = vd.linebuf;

    /* background plane */
    {
        uint16_t name_base = (uint16_t)((vd.regs[2] & 0x0F) << 10);
        int scroll_x = ((int)vd.regs[8] << 0);
        int scroll_y = (int)vd.regs[9];
        bool hscroll_blank_left = (vd.regs[0] & 0x20) != 0;
        int vcol = (ly - scroll_y) & 0xFF;
        int row = vcol >> 3;
        int fine_y = vcol & 7;

        for (int col = 0; col < 32; col++) {
            int eff_col = col + (scroll_x >> 3);
            int cell_x = eff_col & 0x1F;
            uint16_t entry = (uint16_t)(vd.vram[name_base + row * 32 + cell_x] |
                                        ((uint16_t)vd.vram[name_base + row * 32 + cell_x + 0x200] << 8));
            int tile = entry & 0x1FF;
            bool flip_x = (entry & 0x0200) != 0;
            bool flip_y = (entry & 0x0400) != 0;
            uint8_t pal = (uint8_t)(((entry >> 7) & 1) ? 0x10 : 0x00);
            bool priority = (entry & 0x1000) != 0;

            int fy = flip_y ? 7 - fine_y : fine_y;
            const uint8_t *tile_row = vd.vram + ((size_t)tile << 5) + (fy << 2);

            for (int px = 0; px < 8; px++) {
                int sx = col * 8 + px - (scroll_x & 7);
                if (sx < 0 || sx >= w) {
                    continue;
                }
                if (hscroll_blank_left && sx < 8) {
                    lb[sx] = (uint8_t)(bg_color | 0x80); /* mark as backdrop */
                    continue;
                }
                int bit = flip_x ? px : 7 - px;
                int color = 0;
                if (tile_row[0] & (1 << bit)) {
                    color |= 1;
                }
                if (tile_row[1] & (1 << bit)) {
                    color |= 2;
                }
                if (tile_row[2] & (1 << bit)) {
                    color |= 4;
                }
                if (tile_row[3] & (1 << bit)) {
                    color |= 8;
                }
                lb[sx] = color ? (uint8_t)(pal | color) : (uint8_t)(bg_color | 0x80);
            }
        }
    }

    /* sprites */
    if (vd.regs[1] & 0x02) {
        uint16_t sat_base = (uint16_t)((vd.regs[5] & 0x7E) << 7);
        uint16_t spr_base = (uint16_t)((vd.regs[6] & 0x07) << 11);
        int spr_h = (vd.regs[1] & 0x04) ? 16 : 8;
        int spr_w = (vd.regs[0] & 0x08) ? 16 : 8;
        int count = 0;

        for (int i = 0; i < 64 && count < 8; i++) {
            int sy = vd.sat[i * 2] + 1;
            int sx = vd.sat[i * 2 + 1];
            uint8_t meta = vd.vram[sat_base + i * 2 + 1];
            int tile = vd.sat[i * 2 + 2] | ((meta & 0x01) << 8);
            bool flip_x = (meta & 0x02) != 0;
            bool flip_y = (meta & 0x04) != 0;
            uint8_t pal = (uint8_t)(0x10 | (meta & 0x07));
            bool priority = (meta & 0x80) != 0;

            int dy = ly - sy;
            if (dy < 0 || dy >= spr_h) {
                continue;
            }
            count++;

            int fy = flip_y ? spr_h - 1 - dy : dy;
            for (int tx = 0; tx < spr_w; tx += 8) {
                const uint8_t *tile_row = vd.vram + spr_base + (((size_t)tile + (tx >> 3)) << 5) + (fy << 2);
                for (int px = 0; px < 8; px++) {
                    int sxp = sx + tx + px - (sms.hw == SMS_HW_GG ? 48 - 8 : 0);
                    if (sxp < 0 || sxp >= w) {
                        continue;
                    }
                    int bit = flip_x ? px : 7 - px;
                    int color = 0;
                    if (tile_row[0] & (1 << bit)) {
                        color |= 1;
                    }
                    if (tile_row[1] & (1 << bit)) {
                        color |= 2;
                    }
                    if (tile_row[2] & (1 << bit)) {
                        color |= 4;
                    }
                    if (tile_row[3] & (1 << bit)) {
                        color |= 8;
                    }
                    if (!color) {
                        continue;
                    }
                    uint8_t existing = lb[sxp];
                    bool bg_is_backdrop = (existing & 0x80) != 0;
                    if (priority || bg_is_backdrop || !(existing & 0xF)) {
                        lb[sxp] = (uint8_t)(pal | color);
                    } else if (!(meta & 0x20)) {
                        /* sprite-adjacent transparency rule simplified away */
                    }
                }
            }
        }
    }

    /* convert to RGB565 */
    uint16_t *dst = sms.framebuffer + (size_t)y * w;
    for (int x = 0; x < w; x++) {
        dst[x] = vd.rgb_palette[lb[x] & 0x1F];
    }
}

void vdp_tick(int cycles)
{
    vd.line_cycles += cycles;
    while (vd.line_cycles >= CYCLES_PER_LINE) {
        vd.line_cycles -= CYCLES_PER_LINE;

        if (vd.line < VISIBLE_LINES) {
            render_line(vd.line);
        }

        vd.line++;
        if (vd.line >= LINES_PER_FRAME) {
            vd.line = 0;
        }

        /* vblank interrupt at start of blanking */
        if (vd.line == VISIBLE_LINES) {
            vd.status_flags |= 0x80;
            if (vd.regs[1] & 0x20) {
                vd.irq_vblank = true;
            }
        }

        /* line interrupt */
        if (vd.line == (int)vd.regs[10]) {
            vd.status_flags |= 0x40;
            if (vd.regs[0] & 0x10) {
                vd.irq_line = true;
            }
        }
    }
}

void vdp_end_frame(void)
{
    /* nothing extra: all visible lines already rendered by vdp_tick */
}

uint8_t *vdp_vram(void)
{
    return vd.vram;
}

uint8_t *vdp_cram(void)
{
    return vd.cram;
}

uint8_t *vdp_sat(void)
{
    return vd.sat;
}
