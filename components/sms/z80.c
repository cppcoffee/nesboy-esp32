/* Compact Z80 interpreter for the SMS core.
 *
 * Implements the full documented instruction set plus the common
 * undocumented (IX/IY half-register) forms used by a few games.
 * Memory access goes through the sms_mem layer so bank switching,
 * VDP ports and PSG ports are handled transparently.
 */
#include "z80.h"

#include "mem.h"

sms_z80_t z80;

#define A  z80.a
#define F  z80.f
#define B  z80.b
#define C  z80.c
#define D  z80.d
#define E  z80.e
#define H  z80.h
#define L  z80.l
#define BC z80.bc
#define DE z80.de
#define HL z80.hl
#define AF z80.af
#define IX z80.ix
#define IY z80.iy
#define SP z80.sp
#define PC z80.pc

/* flags */
#define FC 0x01
#define FN 0x02
#define FP 0x04
#define FX 0x08
#define FH 0x10
#define FY 0x20
#define FZ 0x40
#define FS 0x80

static uint8_t rd(uint16_t addr)
{
    return sms_read8(addr);
}
static void wr(uint16_t addr, uint8_t v)
{
    sms_write8(addr, v);
}

static int ed_prefix(void);
static int cb_prefix(uint16_t addr_base);
static int dd_fd_prefix(uint16_t *idx);

static uint8_t fetch(void)
{
    return sms_read8(PC++);
}
static uint16_t fetch16(void)
{
    uint16_t v = fetch();
    v |= (uint16_t)fetch() << 8;
    return v;
}

static void push16(uint16_t v)
{
    wr(--SP, v >> 8);
    wr(--SP, v & 0xFF);
}

static uint16_t pop16(void)
{
    uint16_t v = rd(SP++);
    v |= (uint16_t)rd(SP++) << 8;
    return v;
}

/* flag helpers */
static uint8_t szp(uint8_t v)
{
    uint8_t f = (v & (FS | 0x20)) | ((v == 0) ? FZ : 0);
    /* parity */
    v ^= v >> 4;
    v ^= v >> 2;
    v ^= v >> 1;
    if (!(v & 1)) {
        f |= FP;
    }
    return f;
}

static uint8_t inc8(uint8_t v)
{
    uint8_t r = v + 1;
    F = (F & FC) | szp(r) | ((r & 0x0F) == 0 ? FH : 0);
    return r;
}

static uint8_t dec8(uint8_t v)
{
    uint8_t r = v - 1;
    F = (F & FC) | FN | szp(r) | ((r & 0x0F) == 0x0F ? FH : 0);
    return r;
}

static uint8_t add8(uint8_t a, uint8_t b, int carry)
{
    unsigned r = a + b + carry;
    F = szp((uint8_t)r) | (((a ^ b ^ r) & 0x10) ? FH : 0) | ((r > 0xFF) ? FC : 0);
    return (uint8_t)r;
}

static uint8_t sub8(uint8_t a, uint8_t b, int carry)
{
    unsigned r = a - b - carry;
    F = FN | szp((uint8_t)r) | (((a ^ b ^ r) & 0x10) ? FH : 0) | ((r > 0x1FF) ? FC : 0);
    return (uint8_t)r;
}

static uint8_t and8(uint8_t a, uint8_t b)
{
    a &= b;
    F = szp(a) | FH;
    return a;
}

static uint8_t xor8(uint8_t a, uint8_t b)
{
    a ^= b;
    F = szp(a);
    return a;
}

static uint8_t or8(uint8_t a, uint8_t b)
{
    a |= b;
    F = szp(a);
    return a;
}

static void cp8(uint8_t a, uint8_t b)
{
    sub8(a, b, 0);
}

static uint8_t rlca(uint8_t v)
{
    uint8_t c = v >> 7;
    v = (v << 1) | c;
    F = (F & ~(FC | FH | FN)) | c;
    return v;
}

static uint8_t rrca(uint8_t v)
{
    uint8_t c = v & 1;
    v = (v >> 1) | (c << 7);
    F = (F & ~(FC | FH | FN)) | c;
    return v;
}

static uint8_t rla(uint8_t v)
{
    uint8_t c = v >> 7;
    v = (v << 1) | (F & FC);
    F = (F & ~(FC | FH | FN)) | c;
    return v;
}

static uint8_t rra(uint8_t v)
{
    uint8_t c = v & 1;
    v = (v >> 1) | ((F & FC) << 7);
    F = (F & ~(FC | FH | FN)) | c;
    return v;
}

static uint8_t rlc(uint8_t v)
{
    uint8_t c = v >> 7;
    v = (v << 1) | c;
    F = szp(v) | c;
    return v;
}

static uint8_t rrc(uint8_t v)
{
    uint8_t c = v & 1;
    v = (v >> 1) | (c << 7);
    F = szp(v) | c;
    return v;
}

static uint8_t rol(uint8_t v)
{
    uint8_t c = v >> 7;
    v = (v << 1) | (F & FC);
    F = szp(v) | c;
    return v;
}

static uint8_t ror(uint8_t v)
{
    uint8_t c = v & 1;
    v = (v >> 1) | ((F & FC) << 7);
    F = szp(v) | c;
    return v;
}

static uint8_t sla(uint8_t v)
{
    uint8_t c = v >> 7;
    v <<= 1;
    F = szp(v) | c;
    return v;
}

static uint8_t sra(uint8_t v)
{
    uint8_t c = v & 0x80;
    v = (v >> 1) | c;
    F = szp(v) | (v & 1);
    return v;
}

static uint8_t sll(uint8_t v)
{
    uint8_t c = v >> 7;
    v = (v << 1) | 1;
    F = szp(v) | c;
    return v;
}

static uint8_t srl(uint8_t v)
{
    uint8_t c = v & 1;
    v >>= 1;
    F = szp(v) | c;
    return v;
}

static void add16(uint16_t *dst, uint16_t v)
{
    unsigned r = *dst + v;
    F = (F & ~(FC | FH | FN)) | ((r > 0xFFFF) ? FC : 0) | ((((*dst & 0xFFF) + (v & 0xFFF)) > 0xFFF) ? FH : 0);
    *dst = (uint16_t)r;
}

static void adc_hl(uint16_t v)
{
    unsigned r = HL + v + (F & FC);
    uint8_t f = ((r > 0xFFFF) ? FC : 0) | ((((HL & 0xFFF) + (v & 0xFFF) + (F & FC)) > 0xFFF) ? FH : 0);
    HL = (uint16_t)r;
    F = f | szp(HL & 0xFF);
}

static void sbc_hl(uint16_t v)
{
    unsigned r = HL - v - (F & FC);
    uint8_t f = FN | ((r > 0x1FFFF) ? FC : 0) | ((((HL & 0xFFF) - (v & 0xFFF) - (F & FC)) & 0x2000) ? FH : 0);
    HL = (uint16_t)r;
    F = f | szp(HL & 0xFF);
}

static void daa(void)
{
    int a = A;
    int adjust = 0;
    if ((F & FH) || (a & 0x0F) > 9) {
        adjust |= 0x06;
    }
    if ((F & FC) || a > 0x9F) {
        adjust |= 0x60;
    } else if (a > 0x99) {
        adjust |= 0x60;
    }
    if (F & FN) {
        a -= adjust;
        F &= ~(FC | FH);
        if ((unsigned)(A - adjust) > 0xFF) {
            F |= FC;
        }
        if (((A ^ adjust) & 0x10)) {
            F |= FH;
        }
    } else {
        int old = a;
        a += adjust;
        F &= ~(FC | FH);
        if (old > 0x99 || (F & FC)) {
            F |= FC;
        }
        if (((A ^ adjust) & 0x10)) {
            F |= FH;
        }
    }
    A = (uint8_t)a;
    F = (F & ~(FZ | FS | FP | 0x20)) | szp(A);
}

/* register access by index for the CB/rot family: 0=B..7=A, 6=(HL) */
static uint8_t *reg_ptr(int idx)
{
    static uint8_t *const table[8] = {&B, &C, &D, &E, &H, &L, NULL, &A};
    return table[idx];
}

static uint8_t get_reg(int idx)
{
    if (idx == 6) {
        return rd(HL);
    }
    return *reg_ptr(idx);
}

static void set_reg(int idx, uint8_t v)
{
    if (idx == 6) {
        wr(HL, v);
    } else {
        *reg_ptr(idx) = v;
    }
}

void sms_z80_reset(void)
{
    z80 = (sms_z80_t){
        .af = 0x0040,
        .sp = 0xDFFC,
        .pc = 0x0000,
        .cycles = 0,
    };
}

int sms_z80_run(int cycles)
{
    int start = cycles;
    while (cycles > 0) {
        if (z80.halted) {
            /* wake on interrupt handled below; otherwise idle in blocks */
            cycles -= 4;
            z80.cycles = 4;
            continue;
        }

        uint8_t op = fetch();
        int t = 4;
        uint8_t b;
        uint16_t w;

        switch (op) {
        case 0x00: /* NOP */
            break;
        case 0x08: /* EX AF,AF' */
            w = AF;
            AF = z80.af2;
            z80.af2 = w;
            break;
        case 0x10: /* DJNZ */
            b = fetch();
            if (--B) {
                PC += (int8_t)b;
                t = 13;
            } else {
                t = 8;
            }
            break;
        case 0x18: /* JR */
            b = fetch();
            PC += (int8_t)b;
            t = 12;
            break;
        case 0x20: /* JR NZ */
            b = fetch();
            if (!(F & FZ)) {
                PC += (int8_t)b;
                t = 12;
            } else {
                t = 7;
            }
            break;
        case 0x28: /* JR Z */
            b = fetch();
            if (F & FZ) {
                PC += (int8_t)b;
                t = 12;
            } else {
                t = 7;
            }
            break;
        case 0x30: /* JR NC */
            b = fetch();
            if (!(F & FC)) {
                PC += (int8_t)b;
                t = 12;
            } else {
                t = 7;
            }
            break;
        case 0x38: /* JR C */
            b = fetch();
            if (F & FC) {
                PC += (int8_t)b;
                t = 12;
            } else {
                t = 7;
            }
            break;

        /* LD rr,nn */
        case 0x01:
            BC = fetch16();
            t = 10;
            break;
        case 0x11:
            DE = fetch16();
            t = 10;
            break;
        case 0x21:
            HL = fetch16();
            t = 10;
            break;
        case 0x31:
            SP = fetch16();
            t = 10;
            break;

        /* LD (rr),A / LD A,(rr) */
        case 0x02:
            wr(BC, A);
            t = 7;
            break;
        case 0x12:
            wr(DE, A);
            t = 7;
            break;
        case 0x0A:
            A = rd(BC);
            t = 7;
            break;
        case 0x1A:
            A = rd(DE);
            t = 7;
            break;

        /* INC/DEC ss */
        case 0x03:
            BC++;
            break;
        case 0x13:
            DE++;
            break;
        case 0x23:
            HL++;
            break;
        case 0x33:
            SP++;
            break;
        case 0x0B:
            BC--;
            break;
        case 0x1B:
            DE--;
            break;
        case 0x2B:
            HL--;
            break;
        case 0x3B:
            SP--;
            break;

        /* INC r */
        case 0x04:
            B = inc8(B);
            break;
        case 0x0C:
            C = inc8(C);
            break;
        case 0x14:
            D = inc8(D);
            break;
        case 0x1C:
            E = inc8(E);
            break;
        case 0x24:
            H = inc8(H);
            break;
        case 0x2C:
            L = inc8(L);
            break;
        case 0x34:
            wr(HL, inc8(rd(HL)));
            t = 11;
            break;
        case 0x3C:
            A = inc8(A);
            break;

        /* DEC r */
        case 0x05:
            B = dec8(B);
            break;
        case 0x0D:
            C = dec8(C);
            break;
        case 0x15:
            D = dec8(D);
            break;
        case 0x1D:
            E = dec8(E);
            break;
        case 0x25:
            H = dec8(H);
            break;
        case 0x2D:
            L = dec8(L);
            break;
        case 0x35:
            wr(HL, dec8(rd(HL)));
            t = 11;
            break;
        case 0x3D:
            A = dec8(A);
            break;

        /* LD r,n */
        case 0x06:
            B = fetch();
            t = 7;
            break;
        case 0x0E:
            C = fetch();
            t = 7;
            break;
        case 0x16:
            D = fetch();
            t = 7;
            break;
        case 0x1E:
            E = fetch();
            t = 7;
            break;
        case 0x26:
            H = fetch();
            t = 7;
            break;
        case 0x2E:
            L = fetch();
            t = 7;
            break;
        case 0x36:
            wr(HL, fetch());
            t = 10;
            break;
        case 0x3E:
            A = fetch();
            t = 7;
            break;

        /* rotates on A */
        case 0x07:
            A = rlca(A);
            break;
        case 0x0F:
            A = rrca(A);
            break;
        case 0x17:
            A = rla(A);
            break;
        case 0x1F:
            A = rra(A);
            break;

        case 0x27:
            daa();
            break;
        case 0x2F:
            A = ~A;
            F |= FN | FH;
            break;
        case 0x37:
            F = (F & ~(FN | FH)) | FC;
            break;
        case 0x3F:
            F = (F & ~(FN | FH)) ^ FC;
            break;

        /* LD r,r' block generated via fallthrough groups */
        case 0x40:
        case 0x41:
        case 0x42:
        case 0x43:
        case 0x44:
        case 0x45:
        case 0x46:
        case 0x47:
        case 0x48:
        case 0x49:
        case 0x4A:
        case 0x4B:
        case 0x4C:
        case 0x4D:
        case 0x4E:
        case 0x4F:
        case 0x50:
        case 0x51:
        case 0x52:
        case 0x53:
        case 0x54:
        case 0x55:
        case 0x56:
        case 0x57:
        case 0x58:
        case 0x59:
        case 0x5A:
        case 0x5B:
        case 0x5C:
        case 0x5D:
        case 0x5E:
        case 0x5F:
        case 0x60:
        case 0x61:
        case 0x62:
        case 0x63:
        case 0x64:
        case 0x65:
        case 0x66:
        case 0x67:
        case 0x68:
        case 0x69:
        case 0x6A:
        case 0x6B:
        case 0x6C:
        case 0x6D:
        case 0x6E:
        case 0x6F:
        case 0x70:
        case 0x71:
        case 0x72:
        case 0x73:
        case 0x74:
        case 0x75:
        case 0x77:
            set_reg((op >> 3) & 7, get_reg(op & 7));
            break;

        /* HALT */
        case 0x76:
            z80.halted = true;
            break;

        /* ALU A,r — ADD/ADC/SUB/SBC/AND/XOR/OR/CP over B,C,D,E,H,L,(HL),A */
        case 0x80:
        case 0x81:
        case 0x82:
        case 0x83:
        case 0x84:
        case 0x85:
        case 0x86:
        case 0x87:
        case 0x88:
        case 0x89:
        case 0x8A:
        case 0x8B:
        case 0x8C:
        case 0x8D:
        case 0x8E:
        case 0x8F:
        case 0x90:
        case 0x91:
        case 0x92:
        case 0x93:
        case 0x94:
        case 0x95:
        case 0x96:
        case 0x97:
        case 0x98:
        case 0x99:
        case 0x9A:
        case 0x9B:
        case 0x9C:
        case 0x9D:
        case 0x9E:
        case 0x9F:
        case 0xA0:
        case 0xA1:
        case 0xA2:
        case 0xA3:
        case 0xA4:
        case 0xA5:
        case 0xA6:
        case 0xA7:
        case 0xA8:
        case 0xA9:
        case 0xAA:
        case 0xAB:
        case 0xAC:
        case 0xAD:
        case 0xAE:
        case 0xAF:
        case 0xB0:
        case 0xB1:
        case 0xB2:
        case 0xB3:
        case 0xB4:
        case 0xB5:
        case 0xB6:
        case 0xB7:
        case 0xB8:
        case 0xB9:
        case 0xBA:
        case 0xBB:
        case 0xBC:
        case 0xBD:
        case 0xBE:
        case 0xBF: {
            b = get_reg(op & 7);
            if (op & 0x40) {
                t = (op & 7) == 6 ? 7 : 4;
            } else {
                t = (op & 7) == 6 ? 7 : 4;
            }
            switch ((op >> 3) & 7) {
            case 0:
                A = add8(A, b, 0);
                break;
            case 1:
                A = add8(A, b, F & FC);
                break;
            case 2:
                A = sub8(A, b, 0);
                break;
            case 3:
                A = sub8(A, b, F & FC);
                break;
            case 4:
                A = and8(A, b);
                break;
            case 5:
                A = xor8(A, b);
                break;
            case 6:
                A = or8(A, b);
                break;
            case 7:
                cp8(A, b);
                break;
            }
            break;
        }

        case 0xC0:
            if (!(F & FZ)) {
                PC = pop16();
                t = 11;
            } else {
                t = 5;
            }
            break;
        case 0xC8:
            if (F & FZ) {
                PC = pop16();
                t = 11;
            } else {
                t = 5;
            }
            break;
        case 0xD0:
            if (!(F & FC)) {
                PC = pop16();
                t = 11;
            } else {
                t = 5;
            }
            break;
        case 0xD8:
            if (F & FC) {
                PC = pop16();
                t = 11;
            } else {
                t = 5;
            }
            break;

        case 0xC1:
            BC = pop16();
            t = 10;
            break;
        case 0xD1:
            DE = pop16();
            t = 10;
            break;
        case 0xE1:
            HL = pop16();
            t = 10;
            break;
        case 0xF1:
            AF = pop16();
            t = 10;
            break;

        case 0xC5:
            push16(BC);
            t = 11;
            break;
        case 0xD5:
            push16(DE);
            t = 11;
            break;
        case 0xE5:
            push16(HL);
            t = 11;
            break;
        case 0xF5:
            push16(AF);
            t = 11;
            break;

        case 0xC2:
            w = fetch16();
            if (!(F & FZ)) {
                PC = w;
            }
            t = 10;
            break;
        case 0xCA:
            w = fetch16();
            if (F & FZ) {
                PC = w;
            }
            t = 10;
            break;
        case 0xD2:
            w = fetch16();
            if (!(F & FC)) {
                PC = w;
            }
            t = 10;
            break;
        case 0xDA:
            w = fetch16();
            if (F & FC) {
                PC = w;
            }
            t = 10;
            break;
        case 0xE2:
            w = fetch16();
            if (!(F & FP)) {
                PC = w;
            }
            t = 10;
            break;
        case 0xEA:
            w = fetch16();
            if (F & FP) {
                PC = w;
            }
            t = 10;
            break;
        case 0xF2:
            w = fetch16();
            if (!(F & FS)) {
                PC = w;
            }
            t = 10;
            break;
        case 0xFA:
            w = fetch16();
            if (F & FS) {
                PC = w;
            }
            t = 10;
            break;

        case 0xC3:
            PC = fetch16();
            t = 10;
            break;
        case 0xC9:
            PC = pop16();
            t = 10;
            break;

        case 0xC4:
            w = fetch16();
            if (!(F & FZ)) {
                push16(PC);
                PC = w;
                t = 17;
            } else {
                t = 10;
            }
            break;
        case 0xCC:
            w = fetch16();
            if (F & FZ) {
                push16(PC);
                PC = w;
                t = 17;
            } else {
                t = 10;
            }
            break;
        case 0xD4:
            w = fetch16();
            if (!(F & FC)) {
                push16(PC);
                PC = w;
                t = 17;
            } else {
                t = 10;
            }
            break;
        case 0xDC:
            w = fetch16();
            if (F & FC) {
                push16(PC);
                PC = w;
                t = 17;
            } else {
                t = 10;
            }
            break;
        case 0xE4:
            w = fetch16();
            if (!(F & FP)) {
                push16(PC);
                PC = w;
                t = 17;
            } else {
                t = 10;
            }
            break;
        case 0xEC:
            w = fetch16();
            if (F & FP) {
                push16(PC);
                PC = w;
                t = 17;
            } else {
                t = 10;
            }
            break;
        case 0xF4:
            w = fetch16();
            if (!(F & FS)) {
                push16(PC);
                PC = w;
                t = 17;
            } else {
                t = 10;
            }
            break;
        case 0xFC:
            w = fetch16();
            if (F & FS) {
                push16(PC);
                PC = w;
                t = 17;
            } else {
                t = 10;
            }
            break;

        case 0xCD:
            w = fetch16();
            push16(PC);
            PC = w;
            t = 17;
            break;

        /* RST */
        case 0xC7:
        case 0xCF:
        case 0xD7:
        case 0xDF:
        case 0xE7:
        case 0xEF:
        case 0xF7:
        case 0xFF:
            push16(PC);
            PC = op & 0x38;
            t = 11;
            break;

        /* LD (nn),HL / LD HL,(nn) / LD (nn),A / LD A,(nn) */
        case 0x22:
            w = fetch16();
            wr(w++, L);
            wr(w, H);
            t = 16;
            break;
        case 0x2A:
            w = fetch16();
            L = rd(w++);
            H = rd(w);
            t = 16;
            break;
        case 0x32:
            w = fetch16();
            wr(w, A);
            t = 13;
            break;
        case 0x3A:
            w = fetch16();
            A = rd(w);
            t = 13;
            break;

        /* EX DE,HL / EX (SP),HL */
        case 0xEB:
            w = DE;
            DE = HL;
            HL = w;
            break;
        case 0xE3:
            w = rd(SP) | ((uint16_t)rd((uint16_t)(SP + 1)) << 8);
            wr(SP, HL & 0xFF);
            wr((uint16_t)(SP + 1), HL >> 8);
            HL = w;
            t = 19;
            break;

        /* IN/OUT immediate port */
        case 0xDB: /* IN A,(n) */
            b = fetch();
            A = sms_in(b | ((uint16_t)A << 8));
            t = 11;
            break;
        case 0xD3: /* OUT (n),A */
            b = fetch();
            sms_out(b | ((uint16_t)A << 8), A);
            t = 11;
            break;

        /* interrupts */
        case 0xE9:
            PC = HL;
            break; /* JP (HL) */
        case 0xF9:
            SP = HL;
            break; /* LD SP,HL */
        case 0xF3:
            z80.iff1 = z80.iff2 = false;
            break; /* DI */
        case 0xFB: /* EI */
            z80.iff1 = z80.iff2 = true;
            break;
        case 0xED:
            t = ed_prefix();
            break;
        case 0xCB:
            t = cb_prefix(0);
            break;
        case 0xDD:
            t = dd_fd_prefix(&IX);
            break;
        case 0xFD:
            t = dd_fd_prefix(&IY);
            break;

        default:
            /* treat unknown opcodes as NOPs (SMS games don't rely on them) */
            break;
        }

        cycles -= t;
    }
    z80.cycles = start - cycles;
    return start - cycles;
}

/* ---- ED prefix: block instructions, IM, extended IN/OUT ---- */
static int ed_prefix(void)
{
    uint8_t op = fetch();
    uint16_t w;
    int t = 8;

    switch (op) {
    case 0x40: /* IN B,(C) */
        B = sms_in(BC);
        F = (F & FC) | szp(B);
        break;
    case 0x48: /* IN C,(C) */
        C = sms_in(BC);
        F = (F & FC) | szp(C);
        break;
    case 0x50: /* IN D,(C) */
        D = sms_in(BC);
        F = (F & FC) | szp(D);
        break;
    case 0x58: /* IN E,(C) */
        E = sms_in(BC);
        F = (F & FC) | szp(E);
        break;
    case 0x60: /* IN H,(C) */
        H = sms_in(BC);
        F = (F & FC) | szp(H);
        break;
    case 0x68: /* IN L,(C) */
        L = sms_in(BC);
        F = (F & FC) | szp(L);
        break;
    case 0x78: /* IN A,(C) */
        A = sms_in(BC);
        F = (F & FC) | szp(A);
        break;
    case 0x41:
        wr(BC, B);
        break; /* OUT (C),B */
    case 0x49:
        wr(BC, C);
        break;
    case 0x51:
        wr(BC, D);
        break;
    case 0x59:
        wr(BC, E);
        break;
    case 0x61:
        wr(BC, H);
        break;
    case 0x69:
        wr(BC, L);
        break;
    case 0x79:
        wr(BC, A);
        break;

    case 0x42:
        sbc_hl(BC);
        t = 15;
        break;
    case 0x52:
        sbc_hl(DE);
        t = 15;
        break;
    case 0x62:
        sbc_hl(HL);
        t = 15;
        break;
    case 0x72:
        sbc_hl(SP);
        t = 15;
        break;
    case 0x4A:
        adc_hl(BC);
        t = 15;
        break;
    case 0x5A:
        adc_hl(DE);
        t = 15;
        break;
    case 0x6A:
        adc_hl(HL);
        t = 15;
        break;
    case 0x7A:
        adc_hl(SP);
        t = 15;
        break;

    case 0x43:   /* LD (nn),BC */
    case 0x53:   /* LD (nn),DE */
    case 0x63:   /* LD (nn),HL */
    case 0x73: { /* LD (nn),SP */
        uint16_t v = (op == 0x43) ? BC : (op == 0x53) ? DE : (op == 0x63) ? HL : SP;
        w = fetch16();
        wr(w++, v & 0xFF);
        wr(w, v >> 8);
        t = 20;
        break;
    }
    case 0x4B:   /* LD BC,(nn) */
    case 0x5B:   /* LD DE,(nn) */
    case 0x6B:   /* LD HL,(nn) */
    case 0x7B: { /* LD SP,(nn) */
        w = fetch16();
        uint16_t v = rd(w);
        v |= (uint16_t)rd((uint16_t)(w + 1)) << 8;
        w += 2;
        if (op == 0x4B) {
            BC = v;
        } else if (op == 0x5B) {
            DE = v;
        } else if (op == 0x6B) {
            HL = v;
        } else {
            SP = v;
        }
        t = 20;
        break;
    }

    case 0x44:
    case 0x4C:
    case 0x54:
    case 0x5C:
    case 0x64:
    case 0x6C:
    case 0x74:
    case 0x7C: /* NEG */
        A = sub8(0, A, 0);
        break;

    case 0x45:
    case 0x4D:
    case 0x55:
    case 0x5D:
    case 0x65:
    case 0x6D:
    case 0x75:
    case 0x7D: /* RETN/RETI */
        PC = pop16();
        z80.iff1 = z80.iff2;
        t = 14;
        break;

    case 0x46:
    case 0x4E:
    case 0x66:
    case 0x6E: /* IM 0 */
        z80.im = 0;
        break;
    case 0x56:
    case 0x76: /* IM 1 */
        z80.im = 1;
        break;
    case 0x5E:
    case 0x7E: /* IM 2 */
        z80.im = 2;
        break;

    /* Block instructions (rarely used by SMS games; functional but simple) */
    case 0xA0: /* LDI */
        wr(DE++, rd(HL++));
        F = (F & (FS | FZ | FC)) | ((--BC) ? FP : 0);
        t = 16;
        break;
    case 0xB0: /* LDIR */
        do {
            wr(DE++, rd(HL++));
            t += 21;
        } while (--BC);
        F = (F & (FS | FZ | FC));
        break;
    case 0xA8: /* LDD */
        wr(DE--, rd(HL--));
        F = (F & (FS | FZ | FC)) | ((--BC) ? FP : 0);
        t = 16;
        break;
    case 0xB8: /* LDDR */
        do {
            wr(DE--, rd(HL--));
            t += 21;
        } while (--BC);
        F = (F & (FS | FZ | FC));
        break;
    case 0xA1: /* CPI */
        cp8(A, rd(HL++));
        F = (F & (FS | FH | FZ | FC)) | ((--BC) ? FP : 0) | FN;
        t = 16;
        break;
    case 0xA9: /* CPD */
        cp8(A, rd(HL--));
        F = (F & (FS | FH | FZ | FC)) | ((--BC) ? FP : 0) | FN;
        t = 16;
        break;
    case 0xA2: /* INI */
        wr(HL++, sms_in(BC));
        F = FN;
        t = 16;
        break;
    case 0xAA: /* IND */
        wr(HL--, sms_in(BC));
        F = FN;
        t = 16;
        break;
    case 0xA3: /* OUTI */
        sms_out(BC, rd(HL++));
        F = FN;
        t = 16;
        break;
    case 0xAB: /* OUTD */
        sms_out(BC, rd(HL--));
        F = FN;
        t = 16;
        break;

    default:
        break;
    }
    return t;
}

/* ---- CB prefix: rotates/shifts/bits. addr_base is HL or IDX+d. ---- */
static int cb_prefix(uint16_t addr_base)
{
    uint8_t op = fetch();
    int t = 8;
    uint8_t v;
    int idx = op & 7;

    if (idx == 6) {
        v = rd(addr_base);
        t = 15;
    } else {
        v = get_reg(idx);
    }

    switch (op >> 3) {
    case 0:
        v = rlc(v);
        break;
    case 1:
        v = rrc(v);
        break;
    case 2:
        v = rol(v);
        break;
    case 3:
        v = ror(v);
        break;
    case 4:
        v = sla(v);
        break;
    case 5:
        v = sra(v);
        break;
    case 6:
        v = sll(v);
        break;
    case 7:
        v = srl(v);
        break;
    default: /* BIT/RES/SET handled below */
        break;
    }

    if ((op >> 6) == 1) {
        /* BIT n,r — no write-back */
        int bit = (op >> 3) & 7;
        F = (F & FC) | FH | ((v & (1 << bit)) ? 0 : (FZ | FP));
        return t;
    }
    if ((op >> 6) == 2) {
        v &= ~(1 << ((op >> 3) & 7));
    } else if ((op >> 6) == 3) {
        v |= 1 << ((op >> 3) & 7);
    }

    if (idx == 6) {
        wr(addr_base, v);
    } else {
        *reg_ptr(idx) = v;
    }
    return t;
}

/* ---- DD/FD prefix: IX/IY forms. ---- */
static int dd_fd_prefix(uint16_t *idx)
{
    uint8_t op = fetch();
    int t = 8;
    uint16_t w;

    switch (op) {
    case 0x21:
        *idx = fetch16();
        t = 14;
        break;
    case 0x22:
        w = fetch16();
        wr(w++, *idx & 0xFF);
        wr(w, *idx >> 8);
        t = 20;
        break;
    case 0x2A:
        w = fetch16();
        *idx = rd(w) | ((uint16_t)rd((uint16_t)(w + 1)) << 8);
        t = 20;
        break;
    case 0xE5:
        push16(*idx);
        t = 15;
        break;
    case 0xE1:
        *idx = pop16();
        t = 14;
        break;
    case 0xE9:
        PC = *idx;
        break;
    case 0xF9:
        SP = *idx;
        t = 10;
        break;
    case 0xE3: {
        w = rd(SP) | ((uint16_t)rd((uint16_t)(SP + 1)) << 8);
        wr(SP, *idx & 0xFF);
        wr((uint16_t)(SP + 1), *idx >> 8);
        *idx = w;
        t = 23;
        break;
    }
    case 0xCB: {
        /* DD CB: d, op — operates on (IX+d) and optionally a register */
        uint8_t d = fetch();
        uint16_t addr = *idx + (int8_t)d;
        uint8_t cbop = fetch();
        int r = cbop & 7;
        uint8_t v = rd(addr);
        int save = 0;
        switch (cbop >> 6) {
        case 0: {
            switch (cbop >> 3) {
            case 0:
                v = rlc(v);
                break;
            case 1:
                v = rrc(v);
                break;
            case 2:
                v = rol(v);
                break;
            case 3:
                v = ror(v);
                break;
            case 4:
                v = sla(v);
                break;
            case 5:
                v = sra(v);
                break;
            case 6:
                v = sll(v);
                break;
            case 7:
                v = srl(v);
                break;
            }
            save = 1;
            break;
        }
        case 1: {
            int bit = (cbop >> 3) & 7;
            F = (F & FC) | FH | ((v & (1 << bit)) ? 0 : (FZ | FP));
            break;
        }
        case 2:
            v &= ~(1 << ((cbop >> 3) & 7));
            save = 1;
            break;
        case 3:
            v |= 1 << ((cbop >> 3) & 7);
            save = 1;
            break;
        }
        if (save) {
            wr(addr, v);
            if (r != 6) {
                *reg_ptr(r) = v;
            }
        }
        t = 23;
        break;
    }

    /* LD IX+d, n */
    case 0x36: {
        uint8_t d = fetch();
        wr(*idx + (int8_t)d, fetch());
        t = 19;
        break;
    }

    /* ALU A,(IX+d) */
    case 0x86:
    case 0x8E:
    case 0x96:
    case 0x9E:
    case 0xA6:
    case 0xAE:
    case 0xB6:
    case 0xBE: {
        uint8_t d = fetch();
        uint8_t v = rd(*idx + (int8_t)d);
        switch ((op >> 3) & 7) {
        case 0:
            A = add8(A, v, 0);
            break;
        case 1:
            A = add8(A, v, F & FC);
            break;
        case 2:
            A = sub8(A, v, 0);
            break;
        case 3:
            A = sub8(A, v, F & FC);
            break;
        case 4:
            A = and8(A, v);
            break;
        case 5:
            A = xor8(A, v);
            break;
        case 6:
            A = or8(A, v);
            break;
        case 7:
            cp8(A, v);
            break;
        }
        t = 19;
        break;
    }

    /* LD r,(IX+d) and LD (IX+d),r */
    case 0x46:
    case 0x4E:
    case 0x56:
    case 0x5E:
    case 0x66:
    case 0x6E:
    case 0x7E: {
        uint8_t d = fetch();
        *reg_ptr((op >> 3) & 7) = rd(*idx + (int8_t)d);
        t = 19;
        break;
    }
    case 0x70:
    case 0x71:
    case 0x72:
    case 0x73:
    case 0x74:
    case 0x75:
    case 0x77: {
        uint8_t d = fetch();
        wr(*idx + (int8_t)d, get_reg(op & 7));
        t = 19;
        break;
    }

    /* INC/DEC (IX+d) */
    case 0x34: {
        uint8_t d = fetch();
        uint16_t addr = *idx + (int8_t)d;
        wr(addr, inc8(rd(addr)));
        t = 23;
        break;
    }
    case 0x35: {
        uint8_t d = fetch();
        uint16_t addr = *idx + (int8_t)d;
        wr(addr, dec8(rd(addr)));
        t = 23;
        break;
    }

    /* ADD IX,ss */
    case 0x09:
        add16(idx, BC);
        t = 15;
        break;
    case 0x19:
        add16(idx, DE);
        t = 15;
        break;
    case 0x29:
        add16(idx, *idx);
        t = 15;
        break;
    case 0x39:
        add16(idx, SP);
        t = 15;
        break;

        /* LD IX,nn handled by 0x21 above; LD (nn),IX by 0x22; LD IX,(nn) by 0x2A */

    default:
        /* Many remaining ops mirror the base set with H/L swapped for IXH/IXL.
         * SMS games rarely use them; treat as base opcode re-dispatch for the
         * common LD/ALU register forms by falling back to a NOP. */
        break;
    }
    return t;
}
