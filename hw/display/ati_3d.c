/*
 * QEMU ATI SVGA emulation
 * Radeon R100 and R200 3D engine (rasterizer subset)
 *
 * Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
 *
 * This work is licensed under the GNU GPL license version 2 or later.
 */

/*
 * This draws the primitives a desktop needs from the 3D engine, with the
 * vertices already in window coordinates (no transform and lighting):
 * triangles, strips, fans, rectangles and quads, with gouraud colour, up to
 * two textures, the R100 texture combiners, alpha test and alpha blending.
 * MorphOS draws its anti-aliased text and translucent parts this way.
 * The R200 (the Mobility M9 of the Sam440ep) has the same rasterizer,
 * texture formats and blending, but its own vertex format registers,
 * texture unit registers and register combiners; AmigaOS 4.1 composites
 * its windows and the AmiDock with them.
 * There is no depth or stencil buffer, no fog and no mipmapping yet.
 */

#include "qemu/osdep.h"
#include <math.h>
#include "qemu/log.h"
#include "ati_int.h"
#include "ati_regs.h"
#include "trace.h"

/* registers of the 3D engine, stored in the register file */
#define R3D_BLENDCNTL           0x1c20
#define R3D_ZSTENCILCNTL        0x1c2c
#define R3D_PP_CNTL             0x1c38
#define R3D_CNTL                0x1c3c
#define R3D_COLOROFFSET         0x1c40
#define R3D_RE_WIDTH_HEIGHT     0x1c44
#define R3D_COLORPITCH          0x1c48
#define R3D_SE_CNTL             0x1c4c
#define R3D_SE_COORD_FMT        0x1c50
#define R3D_PP_TXFILTER(n)      (0x1c54 + (n) * 0x18)
#define R3D_PP_TXFORMAT(n)      (0x1c58 + (n) * 0x18)
#define R3D_PP_TXOFFSET(n)      (0x1c5c + (n) * 0x18)
#define R3D_PP_TXCBLEND(n)      (0x1c60 + (n) * 0x18)
#define R3D_PP_TXABLEND(n)      (0x1c64 + (n) * 0x18)
#define R3D_PP_TFACTOR(n)       (0x1c68 + (n) * 0x18)
#define R3D_PP_MISC             0x1c14
#define R3D_PP_TEX_SIZE(n)      (0x1d04 + (n) * 8)
#define R3D_PP_TEX_PITCH(n)     (0x1d08 + (n) * 8)
#define R3D_PLANEMASK           0x1d84
#define R3D_VPORT_XSCALE        0x1d98
#define R3D_VPORT_XOFFSET       0x1d9c
#define R3D_VPORT_YSCALE        0x1da0
#define R3D_VPORT_YOFFSET       0x1da4
#define R3D_RE_TOP_LEFT         0x26c0
#define R3D_SE_VTX_FMT          0x2080

/* R200 */
#define R200_PP_CNTL_ALT        0x2cc0
#define R200_SE_VTE_CNTL        0x20b0
#define R200_VTX_ST_DENORM      BIT(12)
#define R200_SE_VTX_FMT_0       0x2088
#define R200_SE_VTX_FMT_1       0x208c
#define R200_PP_TXFILTER(n)     (0x2c00 + (n) * 0x20)
#define R200_PP_TXFORMAT(n)     (0x2c04 + (n) * 0x20)
#define R200_PP_TXSIZE(n)       (0x2c0c + (n) * 0x20)
#define R200_PP_TXPITCH(n)      (0x2c10 + (n) * 0x20)
#define R200_PP_TXOFFSET(n)     (0x2d00 + (n) * 0x18)
#define R200_PP_TFACTOR(n)      (0x2ee0 + (n) * 4)
#define R200_PP_TXCBLEND(n)     (0x2f00 + (n) * 0x10)
#define R200_PP_TXCBLEND2(n)    (0x2f04 + (n) * 0x10)
#define R200_PP_TXABLEND(n)     (0x2f08 + (n) * 0x10)
#define R200_PP_TXABLEND2(n)    (0x2f0c + (n) * 0x10)

/* R200 SE_VTX_FMT_0 */
#define R200_VTX_Z0             BIT(0)
#define R200_VTX_W0             BIT(1)
#define R200_VTX_WEIGHTS(f)     (((f) >> 2) & 7)
#define R200_VTX_N0             BIT(6)
#define R200_VTX_POINT_SIZE     BIT(7)
#define R200_VTX_DISCRETE_FOG   BIT(8)
#define R200_VTX_SHININESS_0    BIT(9)
#define R200_VTX_SHININESS_1    BIT(10)
#define R200_VTX_COLOR(f, n)    (((f) >> (11 + (n) * 2)) & 3)

/* R200 combiner arguments and controls */
#define R200_TX_COMP(i)         BIT(16 + (i) * 4)
#define R200_TX_BIAS(i)         BIT(17 + (i) * 4)
#define R200_TX_SCALE(i)        BIT(18 + (i) * 4)
#define R200_TX_NEG(i)          BIT(19 + (i) * 4)
#define R200_TX_OP(v)           (((v) >> 28) & 7)
#define R200_TX2_SCALE(v)       (((v) >> 8) & 7)
#define R200_TX2_CLAMP(v)       (((v) >> 12) & 3)
#define R200_TX2_OUTPUT_REG(v)  (((v) >> 16) & 7)
#define R200_TX2_REPL(v, i)     (((v) >> (26 + (i) * 2)) & 3)

/* PP_CNTL */
#define PP_TEX_ENABLE(n)        BIT(4 + (n))
#define PP_TEX_BLEND_ENABLE(n)  BIT(12 + (n))
#define PP_ALPHA_TEST_ENABLE    BIT(23)

/* RB3D_CNTL */
#define RB_ALPHA_BLEND_ENABLE   BIT(0)
#define RB_PLANE_MASK_ENABLE    BIT(1)
#define RB_COLOR_FORMAT(c)      (((c) >> 10) & 0xf)

/* SE_CNTL */
#define SE_VPORT_XY_XFORM       BIT(24)
#define SE_PIX_CENTER_OGL       BIT(27)

/* SE_VTX_FMT */
#define VTX_W0                  BIT(0)
#define VTX_FPCOLOR             BIT(1)
#define VTX_FPALPHA             BIT(2)
#define VTX_PKCOLOR             BIT(3)
#define VTX_FPSPEC              BIT(4)
#define VTX_FPFOG               BIT(5)
#define VTX_PKSPEC              BIT(6)
#define VTX_ST0                 BIT(7)
#define VTX_ST1                 BIT(8)
#define VTX_Q1                  BIT(9)
#define VTX_ST2                 BIT(10)
#define VTX_Q2                  BIT(11)
#define VTX_ST3                 BIT(12)
#define VTX_Q3                  BIT(13)
#define VTX_Q0                  BIT(14)
#define VTX_N0                  BIT(18)
#define VTX_Z                   BIT(31)

/* SE_VF_CNTL */
#define VF_PRIM(v)              ((v) & 0xf)
#define VF_COLOR_ORDER_RGBA     BIT(6)
#define VF_NUM_VERTICES(v)      ((v) >> 16)

/* PP_TXFORMAT */
#define TXF_FORMAT(f)           ((f) & 0x1f)
#define TXF_ALPHA_IN_MAP        BIT(6)
#define TXF_NON_POWER2          BIT(7)
#define TXF_WIDTH_LOG2(f)       (((f) >> 8) & 0xf)
#define TXF_HEIGHT_LOG2(f)      (((f) >> 12) & 0xf)

#define ATI_3D_MAX_VERTS        0x4000
#define ATI_3D_UNITS            6       /* R200; the R100 uses two */
#define R100_UNITS              2

typedef struct ATIVertex {
    float x, y, w;
    float c[4];                 /* r, g, b, a */
    float tc[ATI_3D_UNITS][3];  /* s, t, q */
} ATIVertex;

typedef struct ATITexture {
    bool enabled;
    const uint8_t *data;
    uint8_t *copy;              /* data fetched from system memory */
    bool fb_endian;             /* in VRAM, stored as the frame buffer */
    int fmt;
    bool alpha_in_map;
    int bypp;
    int w, h, pitch;
    int swap;
    bool linear;
    int clamp_s, clamp_t;
    bool nonparam;
    int route;                  /* the texture coordinates it uses */
} ATITexture;

typedef struct ATI3DCtx {
    ATIVGAState *s;
    uint8_t *cb;
    uint32_t cb_offset;
    int cb_pitch;               /* in pixels */
    int cb_fmt;
    int cb_bypp;
    bool cb_swap;
    int clip_x0, clip_y0, clip_x1, clip_y1;
    uint32_t rb3d_cntl;
    uint32_t blendcntl;
    uint32_t planemask;
    uint32_t pp_cntl;
    uint32_t pp_misc;
    bool ogl_center;
    bool r200;
    int units;
    ATITexture tex[ATI_3D_UNITS];
    uint32_t dirty_lo, dirty_hi;
} ATI3DCtx;

static inline uint32_t r3d(ATIVGAState *s, uint32_t reg)
{
    return s->mmregs[reg / 4];
}

static inline float u2f(uint32_t v)
{
    union {
        uint32_t u;
        float f;
    } x = { .u = v };
    return x.f;
}

static inline float clampf(float v)
{
    return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
}

/* Pixels and texels */

static uint32_t ati_swap(uint32_t v, int bypp, int swap)
{
    switch (swap) {
    case 1:
        return bypp == 4 ? ((v & 0x00ff00ff) << 8) | ((v >> 8) & 0x00ff00ff) :
               bypp == 2 ? bswap16(v) : v;
    case 2:
        return bypp == 4 ? bswap32(v) : bypp == 2 ? bswap16(v) : v;
    case 3:
        return bypp == 4 ? ror32(v, 16) : v;
    default:
        return v;
    }
}

static uint32_t ati_load(const uint8_t *p, int bypp, bool fb_endian,
                         bool fb_be)
{
    uint32_t v = ldn_le_p(p, bypp);

    if (fb_endian && fb_be) {
        v = bypp == 4 ? bswap32(v) : bypp == 2 ? bswap16(v) : v;
    }
    return v;
}

static void ati_texel_decode(const ATITexture *t, uint32_t v, float out[4])
{
    int a = 255, r, g, b;

    switch (t->fmt) {
    case 0: /* I8 */
    case 8: /* Y8 */
        r = g = b = v & 0xff;
        if (t->alpha_in_map) {
            a = r;
        }
        break;
    case 1: /* AI88 */
        r = g = b = v & 0xff;
        if (t->alpha_in_map) {
            a = (v >> 8) & 0xff;
        }
        break;
    case 2: /* RGB332 */
        r = ((v >> 5) & 7) * 255 / 7;
        g = ((v >> 2) & 7) * 255 / 7;
        b = (v & 3) * 255 / 3;
        break;
    case 3: /* ARGB1555 */
        r = ((v >> 10) & 0x1f) * 255 / 31;
        g = ((v >> 5) & 0x1f) * 255 / 31;
        b = (v & 0x1f) * 255 / 31;
        if (t->alpha_in_map) {
            a = v & 0x8000 ? 255 : 0;
        }
        break;
    case 4: /* RGB565 */
        r = ((v >> 11) & 0x1f) * 255 / 31;
        g = ((v >> 5) & 0x3f) * 255 / 63;
        b = (v & 0x1f) * 255 / 31;
        break;
    case 5: /* ARGB4444 */
        r = ((v >> 8) & 0xf) * 17;
        g = ((v >> 4) & 0xf) * 17;
        b = (v & 0xf) * 17;
        if (t->alpha_in_map) {
            a = ((v >> 12) & 0xf) * 17;
        }
        break;
    case 7: /* RGBA8888 */
        r = (v >> 24) & 0xff;
        g = (v >> 16) & 0xff;
        b = (v >> 8) & 0xff;
        if (t->alpha_in_map) {
            a = v & 0xff;
        }
        break;
    case 6: /* ARGB8888 */
    default:
        r = (v >> 16) & 0xff;
        g = (v >> 8) & 0xff;
        b = v & 0xff;
        if (t->alpha_in_map) {
            a = (v >> 24) & 0xff;
        }
        break;
    }
    out[0] = r / 255.0f;
    out[1] = g / 255.0f;
    out[2] = b / 255.0f;
    out[3] = a / 255.0f;
}

static int ati_tex_wrap(int i, int size, int mode)
{
    switch (mode) {
    case 0: /* wrap */
        i %= size;
        return i < 0 ? i + size : i;
    case 1: /* mirror */
    {
        int p = size * 2;

        i %= p;
        i = i < 0 ? i + p : i;
        return i >= size ? p - 1 - i : i;
    }
    default: /* clamp */
        return i < 0 ? 0 : i >= size ? size - 1 : i;
    }
}

static void ati_tex_fetch(ATI3DCtx *c, const ATITexture *t, int x, int y,
                          float out[4])
{
    const uint8_t *p;
    uint32_t v;

    x = ati_tex_wrap(x, t->w, t->clamp_s);
    y = ati_tex_wrap(y, t->h, t->clamp_t);
    p = t->data + y * t->pitch + x * t->bypp;
    v = ati_load(p, t->bypp, t->fb_endian, c->s->vga.big_endian_fb);
    v = ati_swap(v, t->bypp, t->swap);
    if (t->fmt == 10 || t->fmt == 11) {
        /* packed YUV 4:2:2, two pixels in a dword */
        uint32_t pair = ati_load(t->data + y * t->pitch + (x & ~1) * 2, 4,
                                 t->fb_endian, c->s->vga.big_endian_fb);
        int yy, u, vv;
        float yf;

        pair = ati_swap(pair, 4, t->swap);
        if (t->fmt == 10) { /* VYUY */
            yy = (x & 1) ? (pair >> 24) & 0xff : (pair >> 8) & 0xff;
            u = pair & 0xff;
            vv = (pair >> 16) & 0xff;
        } else { /* YVYU */
            yy = (x & 1) ? (pair >> 16) & 0xff : pair & 0xff;
            vv = (pair >> 8) & 0xff;
            u = (pair >> 24) & 0xff;
        }
        yf = 1.164f * (yy - 16);
        out[0] = clampf((yf + 1.596f * (vv - 128)) / 255.0f);
        out[1] = clampf((yf - 0.813f * (vv - 128) - 0.391f * (u - 128)) /
                        255.0f);
        out[2] = clampf((yf + 2.018f * (u - 128)) / 255.0f);
        out[3] = 1.0f;
        return;
    }
    ati_texel_decode(t, v, out);
}

static void ati_tex_sample(ATI3DCtx *c, const ATITexture *t, float s,
                           float tt, float out[4])
{
    float u = t->nonparam ? s : s * t->w;
    float v = t->nonparam ? tt : tt * t->h;

    if (t->linear) {
        float t00[4], t10[4], t01[4], t11[4];
        float fu = u - 0.5f, fv = v - 0.5f;
        int x0 = (int)floorf(fu), y0 = (int)floorf(fv);
        float ax = fu - x0, ay = fv - y0;
        int i;

        ati_tex_fetch(c, t, x0, y0, t00);
        ati_tex_fetch(c, t, x0 + 1, y0, t10);
        ati_tex_fetch(c, t, x0, y0 + 1, t01);
        ati_tex_fetch(c, t, x0 + 1, y0 + 1, t11);
        for (i = 0; i < 4; i++) {
            out[i] = (t00[i] * (1 - ax) + t10[i] * ax) * (1 - ay) +
                     (t01[i] * (1 - ax) + t11[i] * ax) * ay;
        }
    } else {
        ati_tex_fetch(c, t, (int)floorf(u), (int)floorf(v), out);
    }
}

static int ati_tex_bypp(int fmt)
{
    switch (fmt) {
    case 0:
    case 2:
    case 8:
        return 1;
    case 6:
    case 7:
        return 4;
    default:
        return 2;
    }
}

/* the texture registers have the same layout on R100 and R200 */
static bool ati_tex_setup(ATI3DCtx *c, int n)
{
    ATIVGAState *s = c->s;
    ATITexture *t = &c->tex[n];
    uint32_t fmt = r3d(s, c->r200 ? R200_PP_TXFORMAT(n) : R3D_PP_TXFORMAT(n));
    uint32_t filter = r3d(s, c->r200 ? R200_PP_TXFILTER(n) :
                                       R3D_PP_TXFILTER(n));
    uint32_t offset = r3d(s, c->r200 ? R200_PP_TXOFFSET(n) :
                                       R3D_PP_TXOFFSET(n));
    uint32_t addr = offset & ~0x1f;
    uint32_t size;
    uint32_t start;

    t->fmt = TXF_FORMAT(fmt);
    t->alpha_in_map = fmt & TXF_ALPHA_IN_MAP;
    t->bypp = ati_tex_bypp(t->fmt);
    t->route = c->r200 ? MIN((fmt >> 24) & 7, ATI_3D_UNITS - 1) : n;
    if (fmt & TXF_NON_POWER2) {
        uint32_t sz = r3d(s, c->r200 ? R200_PP_TXSIZE(n) : R3D_PP_TEX_SIZE(n));
        uint32_t pitch = r3d(s, c->r200 ? R200_PP_TXPITCH(n) :
                                          R3D_PP_TEX_PITCH(n));

        t->w = (sz & 0x7ff) + 1;
        t->h = ((sz >> 16) & 0x7ff) + 1;
        t->pitch = (pitch & 0x3fe0) + 32;
    } else {
        t->w = 1 << TXF_WIDTH_LOG2(fmt);
        t->h = 1 << TXF_HEIGHT_LOG2(fmt);
        t->pitch = MAX(t->w * t->bypp, 32);
    }
    t->swap = offset & 3;
    t->linear = filter & 1;
    t->clamp_s = (filter >> 23) & 7;
    t->clamp_t = (filter >> 27) & 7;
    t->clamp_s = t->clamp_s == 0 ? 0 : t->clamp_s == 1 ? 1 : 2;
    t->clamp_t = t->clamp_t == 0 ? 0 : t->clamp_t == 1 ? 1 : 2;
    /* R200: SE_VTE_CNTL says texture coordinates are in texels */
    t->nonparam = c->r200 ? !!(r3d(s, R200_SE_VTE_CNTL) & R200_VTX_ST_DENORM) :
                            !!(r3d(s, R3D_SE_COORD_FMT) & BIT(8 + n));
    size = t->pitch * t->h;

    /* textures in video memory are read in place, others are copied */
    if (ati_gpu_vram_offset(s, addr, &start) &&
        start + size <= s->vga.vram_size) {
        t->data = s->vga.vram_ptr + start;
        t->fb_endian = true;
        t->copy = NULL;
        t->enabled = true;
        return true;
    }
    t->copy = g_try_malloc(size);
    if (!t->copy) {
        return false;
    }
    ati_gpu_rw(s, addr, t->copy, size, false);
    t->data = t->copy;
    t->fb_endian = false;
    t->enabled = true;
    return true;
}

/* Texture combiners */

static void ati_comb_arg_color(int arg, const float cur[4], const float dif[4],
                               const float tf[4], float tx[][4], float out[3])
{
    const float *src;
    bool alpha = arg & 1;

    switch (arg) {
    case 2:
    case 3:
        src = cur;
        break;
    case 4:
    case 5:
        src = dif;
        break;
    case 8:
    case 9:
        src = tf;
        break;
    case 10 ... 13:
        src = tx[(arg - 10) / 2];
        break;
    default:
        out[0] = out[1] = out[2] = 0.0f;
        return;
    }
    if (alpha) {
        out[0] = out[1] = out[2] = src[3];
    } else {
        out[0] = src[0];
        out[1] = src[1];
        out[2] = src[2];
    }
}

static float ati_comb_arg_alpha(int arg, const float cur[4], const float dif[4],
                                const float tf[4], float tx[][4])
{
    switch (arg) {
    case 1:
        return cur[3];
    case 2:
        return dif[3];
    case 4:
        return tf[3];
    case 5:
    case 6:
        return tx[arg - 5][3];
    default:
        return 0.0f;
    }
}

static float ati_comb_op(uint32_t ctl, float a, float b, float cc)
{
    float r;

    switch ((ctl >> 18) & 7) {
    case 1: /* subtract */
        r = a * b - cc;
        break;
    case 2: /* add signed */
        r = a * b + cc - 0.5f;
        break;
    case 3: /* blend */
        r = a * (1.0f - cc) + b * cc;
        break;
    default: /* add */
        r = a * b + cc;
        break;
    }
    r *= 1 << ((ctl >> 21) & 3);
    return clampf(r);
}

static void ati_combine(ATI3DCtx *c, int n, float cur[4], const float dif[4],
                        float tx[][4])
{
    ATIVGAState *s = c->s;
    uint32_t cb = r3d(s, R3D_PP_TXCBLEND(n));
    uint32_t ab = r3d(s, R3D_PP_TXABLEND(n));
    uint32_t tfv = r3d(s, R3D_PP_TFACTOR(n));
    float tf[4] = {
        ((tfv >> 16) & 0xff) / 255.0f, ((tfv >> 8) & 0xff) / 255.0f,
        (tfv & 0xff) / 255.0f, (tfv >> 24) / 255.0f
    };
    float a[3], b[3], cc[3], res[4];
    float aa, ba, ca;
    int i;

    ati_comb_arg_color(cb & 0x1f, cur, dif, tf, tx, a);
    ati_comb_arg_color((cb >> 5) & 0x1f, cur, dif, tf, tx, b);
    ati_comb_arg_color((cb >> 10) & 0x1f, cur, dif, tf, tx, cc);
    for (i = 0; i < 3; i++) {
        if (cb & BIT(15)) {
            a[i] = 1.0f - a[i];
        }
        if (cb & BIT(16)) {
            b[i] = 1.0f - b[i];
        }
        if (cb & BIT(17)) {
            cc[i] = 1.0f - cc[i];
        }
        res[i] = ati_comb_op(cb, a[i], b[i], cc[i]);
    }
    aa = ati_comb_arg_alpha(ab & 0xf, cur, dif, tf, tx);
    ba = ati_comb_arg_alpha((ab >> 4) & 0xf, cur, dif, tf, tx);
    ca = ati_comb_arg_alpha((ab >> 8) & 0xf, cur, dif, tf, tx);
    if (ab & BIT(15)) {
        aa = 1.0f - aa;
    }
    if (ab & BIT(16)) {
        ba = 1.0f - ba;
    }
    if (ab & BIT(17)) {
        ca = 1.0f - ca;
    }
    res[3] = ati_comb_op(ab, aa, ba, ca);
    memcpy(cur, res, sizeof(res));
}

/* R200 register combiners: the textures are in R0-R5, R0 is the result */

static void r200_comb_arg(int arg, int repl, bool alpha_stage, float r[][4],
                          const float dif[4], const float tf[4],
                          const float tf1[4], float out[4])
{
    const float *src;

    switch (arg) {
    case 2:
    case 3:
        src = r[0];
        break;
    case 4:
    case 5:
        src = dif;
        break;
    case 8:
    case 9:
        src = tf;
        break;
    case 10 ... 21:
        src = r[(arg - 10) / 2];
        break;
    case 26:
    case 27:
        src = tf1;
        break;
    default:
        out[0] = out[1] = out[2] = out[3] = 0.0f;
        return;
    }
    if (alpha_stage) {
        /* even arguments are alpha, odd ones blue */
        out[3] = src[arg & 1 ? 2 : 3];
        return;
    }
    if (arg & 1) {
        out[0] = out[1] = out[2] = src[3];
    } else if (repl) {
        out[0] = out[1] = out[2] = src[repl - 1];
    } else {
        memcpy(out, src, 3 * sizeof(float));
    }
}

static float r200_comb_mod(uint32_t ctl, int i, float v)
{
    if (ctl & R200_TX_COMP(i)) {
        v = 1.0f - v;
    }
    if (ctl & R200_TX_BIAS(i)) {
        v -= 0.5f;
    }
    if (ctl & R200_TX_SCALE(i)) {
        v *= 2.0f;
    }
    if (ctl & R200_TX_NEG(i)) {
        v = -v;
    }
    return v;
}

static float r200_comb_out(uint32_t ctl, uint32_t ctl2, const float *a,
                           const float *b, const float *cc)
{
    static const float scale[8] = {
        1.0f, 2.0f, 4.0f, 8.0f, 1.0f, 0.5f, 0.25f, 0.125f
    };
    float va = r200_comb_mod(ctl, 0, *a);
    float vb = r200_comb_mod(ctl, 1, *b);
    float vc = r200_comb_mod(ctl, 2, *cc);
    float r;

    switch (R200_TX_OP(ctl)) {
    case 3: /* lerp */
        r = va * vb + (1.0f - va) * vc;
        break;
    default: /* multiply and add */
        r = va * vb + vc;
        break;
    }
    r *= scale[R200_TX2_SCALE(ctl2)];
    return R200_TX2_CLAMP(ctl2) == 1 ? clampf(r) : r;
}

static void r200_combine(ATI3DCtx *c, int n, float r[][4], const float dif[4])
{
    ATIVGAState *s = c->s;
    uint32_t cb = r3d(s, R200_PP_TXCBLEND(n));
    uint32_t cb2 = r3d(s, R200_PP_TXCBLEND2(n));
    uint32_t ab = r3d(s, R200_PP_TXABLEND(n));
    uint32_t ab2 = r3d(s, R200_PP_TXABLEND2(n));
    float tf[4], tf1[4], arg[3][4], res[4];
    uint32_t tfv;
    int i, k, reg;

    tfv = r3d(s, R200_PP_TFACTOR(cb2 & 7));
    tf[0] = ((tfv >> 16) & 0xff) / 255.0f;
    tf[1] = ((tfv >> 8) & 0xff) / 255.0f;
    tf[2] = (tfv & 0xff) / 255.0f;
    tf[3] = (tfv >> 24) / 255.0f;
    tfv = r3d(s, R200_PP_TFACTOR((cb2 >> 4) & 7));
    tf1[0] = ((tfv >> 16) & 0xff) / 255.0f;
    tf1[1] = ((tfv >> 8) & 0xff) / 255.0f;
    tf1[2] = (tfv & 0xff) / 255.0f;
    tf1[3] = (tfv >> 24) / 255.0f;

    for (k = 0; k < 3; k++) {
        r200_comb_arg((cb >> (k * 5)) & 0x1f, R200_TX2_REPL(cb2, k), false,
                      r, dif, tf, tf1, arg[k]);
    }
    for (i = 0; i < 3; i++) {
        res[i] = r200_comb_out(cb, cb2, &arg[0][i], &arg[1][i], &arg[2][i]);
    }
    for (k = 0; k < 3; k++) {
        r200_comb_arg((ab >> (k * 5)) & 0x1f, 0, true, r, dif, tf, tf1,
                      arg[k]);
    }
    res[3] = r200_comb_out(ab, ab2, &arg[0][3], &arg[1][3], &arg[2][3]);

    /* the colour and the alpha each go to the register they name */
    reg = R200_TX2_OUTPUT_REG(cb2);
    if (reg && reg <= ATI_3D_UNITS && ((cb2 >> 20) & 7) != 7) {
        memcpy(r[reg - 1], res, 3 * sizeof(float));
    }
    reg = R200_TX2_OUTPUT_REG(ab2);
    if (reg && reg <= ATI_3D_UNITS) {
        r[reg - 1][3] = res[3];
    }
}

/* Frame buffer blending and output */

static float ati_blend_factor(int f, const float src[4], const float dst[4],
                              int ch)
{
    switch (f) {
    case 32:
        return 0.0f;
    case 33:
        return 1.0f;
    case 34:
        return src[ch];
    case 35:
        return 1.0f - src[ch];
    case 36:
        return dst[ch];
    case 37:
        return 1.0f - dst[ch];
    case 38:
        return src[3];
    case 39:
        return 1.0f - src[3];
    case 40:
        return dst[3];
    case 41:
        return 1.0f - dst[3];
    case 42:
        return ch == 3 ? 1.0f : MIN(src[3], 1.0f - dst[3]);
    default:
        return 1.0f;
    }
}

static void ati_cb_unpack(int fmt, uint32_t v, float out[4])
{
    switch (fmt) {
    case 3: /* ARGB1555 */
        out[0] = ((v >> 10) & 0x1f) / 31.0f;
        out[1] = ((v >> 5) & 0x1f) / 31.0f;
        out[2] = (v & 0x1f) / 31.0f;
        out[3] = v & 0x8000 ? 1.0f : 0.0f;
        break;
    case 4: /* RGB565 */
        out[0] = ((v >> 11) & 0x1f) / 31.0f;
        out[1] = ((v >> 5) & 0x3f) / 63.0f;
        out[2] = (v & 0x1f) / 31.0f;
        out[3] = 1.0f;
        break;
    case 15: /* ARGB4444 */
        out[0] = ((v >> 8) & 0xf) / 15.0f;
        out[1] = ((v >> 4) & 0xf) / 15.0f;
        out[2] = (v & 0xf) / 15.0f;
        out[3] = ((v >> 12) & 0xf) / 15.0f;
        break;
    default: /* ARGB8888 */
        out[0] = ((v >> 16) & 0xff) / 255.0f;
        out[1] = ((v >> 8) & 0xff) / 255.0f;
        out[2] = (v & 0xff) / 255.0f;
        out[3] = (v >> 24) / 255.0f;
        break;
    }
}

static uint32_t ati_cb_pack(int fmt, const float c[4])
{
    int r = c[0] * 255.0f + 0.5f, g = c[1] * 255.0f + 0.5f;
    int b = c[2] * 255.0f + 0.5f, a = c[3] * 255.0f + 0.5f;

    switch (fmt) {
    case 3:
        return (a >= 128 ? 0x8000 : 0) | (r >> 3) << 10 | (g >> 3) << 5 |
               b >> 3;
    case 4:
        return (r >> 3) << 11 | (g >> 2) << 5 | b >> 3;
    case 15:
        return (a >> 4) << 12 | (r >> 4) << 8 | (g >> 4) << 4 | b >> 4;
    default:
        return (uint32_t)a << 24 | r << 16 | g << 8 | b;
    }
}

static bool ati_alpha_test(ATI3DCtx *c, float a)
{
    int ref = c->pp_misc & 0xff;
    int v = a * 255.0f + 0.5f;

    switch ((c->pp_misc >> 8) & 7) {
    case 0:
        return false;
    case 1:
        return v < ref;
    case 2:
        return v <= ref;
    case 3:
        return v == ref;
    case 4:
        return v >= ref;
    case 5:
        return v > ref;
    case 6:
        return v != ref;
    default:
        return true;
    }
}

static void ati_put_fragment(ATI3DCtx *c, int x, int y, float col[4])
{
    ATIVGAState *s = c->s;
    uint32_t off = c->cb_offset + (y * c->cb_pitch + x) * c->cb_bypp;
    uint8_t *p;
    uint32_t v, old = 0;
    int i;

    if (off + c->cb_bypp > s->vga.vram_size) {
        return;
    }
    p = s->vga.vram_ptr + off;
    if ((c->pp_cntl & PP_ALPHA_TEST_ENABLE) && !ati_alpha_test(c, col[3])) {
        return;
    }
    if (c->rb3d_cntl & (RB_ALPHA_BLEND_ENABLE | RB_PLANE_MASK_ENABLE)) {
        old = ati_load(p, c->cb_bypp, true, s->vga.big_endian_fb);
    }
    if (c->rb3d_cntl & RB_ALPHA_BLEND_ENABLE) {
        float dst[4], out[4];
        int sf = (c->blendcntl >> 16) & 0x3f;
        int df = (c->blendcntl >> 24) & 0x3f;
        int fcn = (c->blendcntl >> 12) & 3;

        ati_cb_unpack(c->cb_fmt, old, dst);
        for (i = 0; i < 4; i++) {
            float a = col[i] * ati_blend_factor(sf, col, dst, i);
            float b = dst[i] * ati_blend_factor(df, col, dst, i);

            out[i] = fcn >= 2 ? a - b : a + b;
            out[i] = clampf(out[i]);
        }
        memcpy(col, out, sizeof(out));
    }
    v = ati_cb_pack(c->cb_fmt, col);
    if (c->rb3d_cntl & RB_PLANE_MASK_ENABLE) {
        v = (v & c->planemask) | (old & ~c->planemask);
    }
    if (s->vga.big_endian_fb) {
        v = c->cb_bypp == 4 ? bswap32(v) : c->cb_bypp == 2 ? bswap16(v) : v;
    }
    stn_le_p(p, c->cb_bypp, v);
    c->dirty_lo = MIN(c->dirty_lo, off);
    c->dirty_hi = MAX(c->dirty_hi, off + c->cb_bypp);
}

static void ati_shade(ATI3DCtx *c, int x, int y, const ATIVertex *v)
{
    float tx[ATI_3D_UNITS][4];
    float cur[4];
    int n;

    memcpy(cur, v->c, sizeof(cur));
    for (n = 0; n < c->units; n++) {
        if (c->tex[n].enabled) {
            const float *tc = v->tc[c->tex[n].route];
            float q = tc[2] ? tc[2] : 1.0f;

            ati_tex_sample(c, &c->tex[n], tc[0] / q, tc[1] / q, tx[n]);
        } else {
            tx[n][0] = tx[n][1] = tx[n][2] = tx[n][3] = 0.0f;
        }
    }
    if (c->r200) {
        /*
         * The stages work on the registers; R0 starts as texture 0. With
         * no stage, the diffuse colour goes through.
         */
        bool staged = false;

        for (n = 0; n < c->units; n++) {
            if (c->pp_cntl & PP_TEX_BLEND_ENABLE(n)) {
                r200_combine(c, n, tx, v->c);
                staged = true;
            }
        }
        if (staged) {
            for (n = 0; n < 4; n++) {
                cur[n] = clampf(tx[0][n]);
            }
        }
    } else {
        for (n = 0; n < c->units; n++) {
            if (c->pp_cntl & PP_TEX_BLEND_ENABLE(n)) {
                ati_combine(c, n, cur, v->c, tx);
            }
        }
    }
    ati_put_fragment(c, x, y, cur);
}

/* Rasterization */

static float ati_edge(const ATIVertex *a, const ATIVertex *b, float x, float y)
{
    return (b->x - a->x) * (y - a->y) - (b->y - a->y) * (x - a->x);
}

static bool ati_top_left(const ATIVertex *a, const ATIVertex *b)
{
    /* with y down and counter-clockwise order in this space */
    return (a->y == b->y && b->x < a->x) || b->y > a->y;
}

static void ati_triangle(ATI3DCtx *c, const ATIVertex *v0,
                         const ATIVertex *v1, const ATIVertex *v2)
{
    const ATIVertex *a = v0, *b = v1, *cc = v2;
    float area = ati_edge(a, b, cc->x, cc->y);
    float off = c->ogl_center ? 0.5f : 0.0f;
    int x0, y0, x1, y1, x, y, i, n;
    bool tl0, tl1, tl2;

    if (area == 0.0f) {
        return;
    }
    if (area < 0) {
        const ATIVertex *t = b;

        b = cc;
        cc = t;
        area = -area;
    }
    x0 = MAX((int)floorf(MIN(MIN(a->x, b->x), cc->x) - off), c->clip_x0);
    y0 = MAX((int)floorf(MIN(MIN(a->y, b->y), cc->y) - off), c->clip_y0);
    x1 = MIN((int)ceilf(MAX(MAX(a->x, b->x), cc->x) - off), c->clip_x1);
    y1 = MIN((int)ceilf(MAX(MAX(a->y, b->y), cc->y) - off), c->clip_y1);
    tl0 = ati_top_left(b, cc);
    tl1 = ati_top_left(cc, a);
    tl2 = ati_top_left(a, b);

    for (y = y0; y <= y1; y++) {
        for (x = x0; x <= x1; x++) {
            float px = x + off, py = y + off;
            float w0 = ati_edge(b, cc, px, py);
            float w1 = ati_edge(cc, a, px, py);
            float w2 = ati_edge(a, b, px, py);
            ATIVertex f;
            float iw;

            if (w0 < 0 || w1 < 0 || w2 < 0 ||
                (w0 == 0 && !tl0) || (w1 == 0 && !tl1) ||
                (w2 == 0 && !tl2)) {
                continue;
            }
            w0 /= area;
            w1 /= area;
            w2 /= area;
            /* perspective correct with the 1/w of the vertices */
            iw = w0 * a->w + w1 * b->w + w2 * cc->w;
            iw = iw ? 1.0f / iw : 1.0f;
            for (i = 0; i < 4; i++) {
                f.c[i] = clampf((w0 * a->c[i] * a->w + w1 * b->c[i] * b->w +
                                 w2 * cc->c[i] * cc->w) * iw);
            }
            for (n = 0; n < ATI_3D_UNITS; n++) {
                for (i = 0; i < 3; i++) {
                    f.tc[n][i] = (w0 * a->tc[n][i] * a->w +
                                  w1 * b->tc[n][i] * b->w +
                                  w2 * cc->tc[n][i] * cc->w) * iw;
                }
            }
            ati_shade(c, x, y, &f);
        }
    }
}

static void ati_point(ATI3DCtx *c, const ATIVertex *v)
{
    int x = (int)floorf(v->x), y = (int)floorf(v->y);

    if (x >= c->clip_x0 && x <= c->clip_x1 &&
        y >= c->clip_y0 && y <= c->clip_y1) {
        ati_shade(c, x, y, v);
    }
}

static void ati_vertex_sub_add(ATIVertex *o, const ATIVertex *a,
                               const ATIVertex *b, const ATIVertex *cc)
{
    int i, n;

    /* o = a - b + c, the fourth corner of a rectangle */
    o->x = a->x - b->x + cc->x;
    o->y = a->y - b->y + cc->y;
    o->w = a->w;
    for (i = 0; i < 4; i++) {
        o->c[i] = a->c[i] - b->c[i] + cc->c[i];
    }
    for (n = 0; n < ATI_3D_UNITS; n++) {
        for (i = 0; i < 3; i++) {
            o->tc[n][i] = a->tc[n][i] - b->tc[n][i] + cc->tc[n][i];
        }
    }
}

static void ati_prims(ATI3DCtx *c, int prim, ATIVertex *v, int n)
{
    int i;

    switch (prim) {
    case 1: /* point list */
        for (i = 0; i < n; i++) {
            ati_point(c, &v[i]);
        }
        break;
    case 4: /* triangle list */
        for (i = 0; i + 2 < n; i += 3) {
            ati_triangle(c, &v[i], &v[i + 1], &v[i + 2]);
        }
        break;
    case 5: /* triangle fan */
    case 15: /* polygon */
        for (i = 1; i + 1 < n; i++) {
            ati_triangle(c, &v[0], &v[i], &v[i + 1]);
        }
        break;
    case 6: /* triangle strip */
        for (i = 0; i + 2 < n; i++) {
            ati_triangle(c, &v[i], &v[i + 1], &v[i + 2]);
        }
        break;
    case 8: /* rectangle list: three corners each */
        for (i = 0; i + 2 < n; i += 3) {
            ATIVertex d;

            ati_vertex_sub_add(&d, &v[i], &v[i + 1], &v[i + 2]);
            ati_triangle(c, &v[i], &v[i + 1], &v[i + 2]);
            ati_triangle(c, &v[i], &v[i + 2], &d);
        }
        break;
    case 13: /* quad list */
        for (i = 0; i + 3 < n; i += 4) {
            ati_triangle(c, &v[i], &v[i + 1], &v[i + 2]);
            ati_triangle(c, &v[i], &v[i + 2], &v[i + 3]);
        }
        break;
    case 14: /* quad strip */
        for (i = 0; i + 3 < n; i += 2) {
            ati_triangle(c, &v[i], &v[i + 1], &v[i + 3]);
            ati_triangle(c, &v[i], &v[i + 3], &v[i + 2]);
        }
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "ati_3d: primitive type %d\n", prim);
        break;
    }
}

/* Vertices */

static int ati_vtx_dwords(uint32_t fmt)
{
    int n = 2;

    n += !!(fmt & VTX_Z) + !!(fmt & VTX_W0);
    n += (fmt & VTX_FPCOLOR ? 3 : 0) + !!(fmt & VTX_FPALPHA) +
         !!(fmt & VTX_PKCOLOR);
    n += (fmt & VTX_FPSPEC ? 3 : 0) + !!(fmt & VTX_FPFOG) +
         !!(fmt & VTX_PKSPEC);
    n += (fmt & VTX_ST0 ? 2 : 0) + !!(fmt & VTX_Q0);
    n += (fmt & VTX_ST1 ? 2 : 0) + !!(fmt & VTX_Q1);
    n += (fmt & VTX_ST2 ? 2 : 0) + !!(fmt & VTX_Q2);
    n += (fmt & VTX_ST3 ? 2 : 0) + !!(fmt & VTX_Q3);
    n += fmt & VTX_N0 ? 3 : 0;
    return n;
}

static void ati_vtx_parse(ATIVGAState *s, uint32_t fmt, uint32_t vf,
                          const uint32_t *d, ATIVertex *v)
{
    uint32_t se_cntl = r3d(s, R3D_SE_CNTL);
    int i = 0, n;

    memset(v, 0, sizeof(*v));
    v->x = u2f(d[i++]);
    v->y = u2f(d[i++]);
    if (fmt & VTX_Z) {
        i++;
    }
    v->w = 1.0f;
    if (fmt & VTX_W0) {
        v->w = u2f(d[i++]);
        if (!v->w) {
            v->w = 1.0f;
        }
    }
    v->c[0] = v->c[1] = v->c[2] = v->c[3] = 1.0f;
    if (fmt & VTX_FPCOLOR) {
        v->c[0] = u2f(d[i++]);
        v->c[1] = u2f(d[i++]);
        v->c[2] = u2f(d[i++]);
    }
    if (fmt & VTX_FPALPHA) {
        v->c[3] = u2f(d[i++]);
    }
    if (fmt & VTX_PKCOLOR) {
        uint32_t p = d[i++];

        if (vf & VF_COLOR_ORDER_RGBA) {
            v->c[0] = (p >> 24) / 255.0f;
            v->c[1] = ((p >> 16) & 0xff) / 255.0f;
            v->c[2] = ((p >> 8) & 0xff) / 255.0f;
            v->c[3] = (p & 0xff) / 255.0f;
        } else {
            v->c[3] = (p >> 24) / 255.0f;
            v->c[0] = ((p >> 16) & 0xff) / 255.0f;
            v->c[1] = ((p >> 8) & 0xff) / 255.0f;
            v->c[2] = (p & 0xff) / 255.0f;
        }
    }
    i += (fmt & VTX_FPSPEC ? 3 : 0) + !!(fmt & VTX_FPFOG) +
         !!(fmt & VTX_PKSPEC);
    for (n = 0; n < 4; n++) {
        static const uint32_t st[4] = { VTX_ST0, VTX_ST1, VTX_ST2, VTX_ST3 };
        static const uint32_t q[4] = { VTX_Q0, VTX_Q1, VTX_Q2, VTX_Q3 };
        float ss = 0, tt = 0, qq = 0;

        if (fmt & st[n]) {
            ss = u2f(d[i++]);
            tt = u2f(d[i++]);
        }
        if (fmt & q[n]) {
            qq = u2f(d[i++]);
        }
        if (n < ATI_3D_UNITS) {
            v->tc[n][0] = ss;
            v->tc[n][1] = tt;
            v->tc[n][2] = qq;
        }
    }
    if (se_cntl & SE_VPORT_XY_XFORM && r3d(s, R3D_VPORT_XSCALE)) {
        v->x = v->x * u2f(r3d(s, R3D_VPORT_XSCALE)) +
               u2f(r3d(s, R3D_VPORT_XOFFSET));
        v->y = v->y * u2f(r3d(s, R3D_VPORT_YSCALE)) +
               u2f(r3d(s, R3D_VPORT_YOFFSET));
    }
    /* vertices carry w, interpolation wants 1/w */
    v->w = 1.0f / v->w;
}

/* R200 vertices: SE_VTX_FMT_0 and SE_VTX_FMT_1 */
static int r200_vtx_dwords(uint32_t fmt0, uint32_t fmt1)
{
    static const int color[4] = { 0, 1, 3, 4 };
    int n = 2, i;

    n += !!(fmt0 & R200_VTX_Z0) + !!(fmt0 & R200_VTX_W0);
    n += R200_VTX_WEIGHTS(fmt0);
    n += fmt0 & R200_VTX_N0 ? 3 : 0;
    n += !!(fmt0 & R200_VTX_POINT_SIZE) + !!(fmt0 & R200_VTX_DISCRETE_FOG);
    n += !!(fmt0 & R200_VTX_SHININESS_0) + !!(fmt0 & R200_VTX_SHININESS_1);
    for (i = 0; i < 8; i++) {
        n += color[R200_VTX_COLOR(fmt0, i)];
    }
    for (i = 0; i < 6; i++) {
        n += (fmt1 >> (i * 3)) & 7;
    }
    return n;
}

static void r200_vtx_parse(uint32_t fmt0, uint32_t fmt1, uint32_t vf,
                           const uint32_t *d, ATIVertex *v)
{
    int i = 0, n, k;

    memset(v, 0, sizeof(*v));
    v->x = u2f(d[i++]);
    v->y = u2f(d[i++]);
    if (fmt0 & R200_VTX_Z0) {
        i++;
    }
    v->w = 1.0f;
    if (fmt0 & R200_VTX_W0) {
        v->w = u2f(d[i++]);
        if (!v->w) {
            v->w = 1.0f;
        }
    }
    i += R200_VTX_WEIGHTS(fmt0);
    i += fmt0 & R200_VTX_N0 ? 3 : 0;
    i += !!(fmt0 & R200_VTX_POINT_SIZE) + !!(fmt0 & R200_VTX_DISCRETE_FOG);
    i += !!(fmt0 & R200_VTX_SHININESS_0) + !!(fmt0 & R200_VTX_SHININESS_1);
    v->c[0] = v->c[1] = v->c[2] = v->c[3] = 1.0f;
    for (n = 0; n < 8; n++) {
        switch (R200_VTX_COLOR(fmt0, n)) {
        case 1: { /* packed */
            uint32_t p = d[i++];

            if (n) {
                break;
            }
            if (vf & VF_COLOR_ORDER_RGBA) {
                v->c[0] = (p >> 24) / 255.0f;
                v->c[1] = ((p >> 16) & 0xff) / 255.0f;
                v->c[2] = ((p >> 8) & 0xff) / 255.0f;
                v->c[3] = (p & 0xff) / 255.0f;
            } else {
                v->c[3] = (p >> 24) / 255.0f;
                v->c[0] = ((p >> 16) & 0xff) / 255.0f;
                v->c[1] = ((p >> 8) & 0xff) / 255.0f;
                v->c[2] = (p & 0xff) / 255.0f;
            }
            break;
        }
        case 2: /* float RGB */
        case 3: /* float RGBA */
            for (k = 0; k < (R200_VTX_COLOR(fmt0, n) == 3 ? 4 : 3); k++) {
                float f = u2f(d[i++]);

                if (!n) {
                    v->c[k] = f;
                }
            }
            break;
        }
    }
    for (n = 0; n < 6; n++) {
        int cnt = (fmt1 >> (n * 3)) & 7;

        for (k = 0; k < cnt; k++) {
            float f = u2f(d[i++]);

            if (k < 3) {
                v->tc[n][k] = f;
            }
        }
    }
    v->w = 1.0f / v->w;
}

static bool ati_3d_setup(ATIVGAState *s, ATI3DCtx *c)
{
    uint32_t top_left = r3d(s, R3D_RE_TOP_LEFT);
    uint32_t wh = r3d(s, R3D_RE_WIDTH_HEIGHT);
    uint32_t pitch = r3d(s, R3D_COLORPITCH);
    int n;

    memset(c, 0, sizeof(*c));
    c->s = s;
    c->r200 = ati_is_r200(s);
    c->units = c->r200 ? ATI_3D_UNITS : R100_UNITS;
    c->rb3d_cntl = r3d(s, R3D_CNTL);
    c->cb_fmt = RB_COLOR_FORMAT(c->rb3d_cntl);
    switch (c->cb_fmt) {
    case 3:
    case 4:
    case 15:
        c->cb_bypp = 2;
        break;
    case 6:
        c->cb_bypp = 4;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "ati_3d: colour format %d\n", c->cb_fmt);
        return false;
    }
    c->cb_offset = ati_fb_offset(s, r3d(s, R3D_COLOROFFSET) & ~0xf);
    c->cb_pitch = pitch & 0x1ff8;
    if (!c->cb_pitch) {
        return false;
    }
    c->clip_x0 = top_left & 0x7ff;
    c->clip_y0 = (top_left >> 16) & 0x7ff;
    c->clip_x1 = wh & 0x7ff;
    c->clip_y1 = (wh >> 16) & 0x7ff;
    if (!wh) {
        c->clip_x1 = c->clip_y1 = 0x7ff;
    }
    c->blendcntl = r3d(s, R3D_BLENDCNTL);
    c->planemask = r3d(s, R3D_PLANEMASK);
    c->pp_cntl = r3d(s, R3D_PP_CNTL);
    if (c->r200) {
        /*
         * AmigaOS's R200 driver writes the texture and stage enables, in
         * the layout of PP_CNTL, to 0x2cc0 and leaves PP_CNTL at 0.
         */
        c->pp_cntl |= r3d(s, R200_PP_CNTL_ALT);
    }
    c->pp_misc = r3d(s, R3D_PP_MISC);
    c->ogl_center = r3d(s, R3D_SE_CNTL) & SE_PIX_CENTER_OGL;
    c->dirty_lo = UINT32_MAX;
    for (n = 0; n < c->units; n++) {
        if (c->pp_cntl & PP_TEX_ENABLE(n)) {
            ati_tex_setup(c, n);
        }
    }
    return true;
}

static void ati_3d_finish(ATI3DCtx *c)
{
    int n;

    for (n = 0; n < ATI_3D_UNITS; n++) {
        g_free(c->tex[n].copy);
    }
    if (c->dirty_lo < c->dirty_hi) {
        memory_region_set_dirty(&c->s->vga.vram, c->dirty_lo,
                                c->dirty_hi - c->dirty_lo);
    }
}

/*
 * Draws the vertices of one packet; fmt and vf as SE_VTX_FMT (SE_VTX_FMT_0
 * on R200) and SE_VF_CNTL
 */
static void ati_3d_draw_vertices(ATIVGAState *s, uint32_t fmt, uint32_t vf,
                                 const uint32_t *d, uint32_t n)
{
    ATI3DCtx c;
    ATIVertex *v;
    bool r200 = ati_is_r200(s);
    uint32_t fmt1 = r3d(s, R200_SE_VTX_FMT_1);
    int size = r200 ? r200_vtx_dwords(fmt, fmt1) : ati_vtx_dwords(fmt);
    int count = MIN(VF_NUM_VERTICES(vf), n / size);
    int i;

    if (!count || !ati_3d_setup(s, &c)) {
        return;
    }
    count = MIN(count, ATI_3D_MAX_VERTS);
    v = g_new(ATIVertex, count);
    for (i = 0; i < count; i++) {
        if (r200) {
            r200_vtx_parse(fmt, fmt1, vf, d + i * size, &v[i]);
        } else {
            ati_vtx_parse(s, fmt, vf, d + i * size, &v[i]);
        }
    }
    trace_ati_3d_draw(VF_PRIM(vf), count, fmt, c.tex[0].enabled);
    ati_prims(&c, VF_PRIM(vf), v, count);
    g_free(v);
    ati_3d_finish(&c);
}

void ati_3d_packet(ATIVGAState *s, uint8_t op, const uint32_t *d, uint32_t n)
{
    switch (op) {
    case 0x29: /* 3D_DRAW_IMMD: vertex format, VF_CNTL, vertices */
        if (n >= 2) {
            ati_3d_draw_vertices(s, d[0], d[1], d + 2, n - 2);
        }
        break;
    case 0x35: /* 3D_DRAW_IMMD_2: VF_CNTL, vertices */
        if (n >= 1) {
            ati_3d_draw_vertices(s, r3d(s, ati_is_r200(s) ? R200_SE_VTX_FMT_0 :
                                                            R3D_SE_VTX_FMT),
                                 d[0], d + 1, n - 1);
        }
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "ati_3d: packet 0x%02x\n", op);
        break;
    }
}
