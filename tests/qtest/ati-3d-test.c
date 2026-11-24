/*
 * QTest testcase for the 3D engine of the ATI Radeon (R100) emulation
 *
 * Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"
#include "libqos/pci.h"
#include "libqos/pci-pc.h"

/*
 * The test sets the 3D state through MMIO and sends the draw packets to the
 * command processor through its PIO aperture, then reads the colour buffer
 * back from the frame buffer BAR. The frame buffer is at GPU address 0.
 */

#define MC_FB_LOCATION          0x0148
#define SURFACE_CNTL            0x0b00
#define SURFACE0_LOWER_BOUND    0x0b04
#define SURFACE0_UPPER_BOUND    0x0b08
#define SURFACE0_INFO           0x0b0c
#define CSQ_APER                0x1000
#define PP_MISC                 0x1c14
#define RB3D_DEPTHOFFSET        0x1c24
#define RB3D_DEPTHPITCH         0x1c28
#define RB3D_ZSTENCILCNTL       0x1c2c
#define PP_CNTL                 0x1c38
#define RB3D_CNTL               0x1c3c
#define RB3D_COLOROFFSET        0x1c40
#define RE_WIDTH_HEIGHT         0x1c44
#define RB3D_COLORPITCH         0x1c48
#define SE_CNTL                 0x1c4c
#define SE_COORD_FMT            0x1c50
#define PP_TXFILTER_0           0x1c54
#define PP_TXFORMAT_0           0x1c58
#define PP_TXOFFSET_0           0x1c5c
#define PP_TXCBLEND_0           0x1c60
#define PP_TXABLEND_0           0x1c64
#define RB3D_STENCILREFMASK     0x1d7c
#define RE_TOP_LEFT             0x26c0

#define CB          0x000000    /* 64 x 64 ARGB8888 */
#define ZB          0x100000    /* 64 x 64 depth */
#define TEX         0x200000
#define VB          0x300000
#define W           64

#define RB_COLOR_ARGB8888   (6 << 10)
#define RB_STENCIL_ENABLE   (1 << 7)
#define RB_Z_ENABLE         (1 << 8)
#define Z_WRITE             (1 << 30)
#define Z_TEST(f)           ((f) << 4)
#define ST_TEST(f)          ((f) << 12)
#define ST_ZPASS(op)        ((op) << 20)
#define FUNC_LESS           1
#define FUNC_EQUAL          3
#define FUNC_ALWAYS         7
#define ST_REPLACE          2
#define DEPTH_16            0
#define DEPTH_24_8          2

/* both faces drawn, gouraud colour and alpha */
#define SE_SOLID            ((3 << 1) | (3 << 3) | (2 << 8) | (2 << 10))
#define SE_CULL_BACK        ((3 << 3) | (2 << 8) | (2 << 10))

#define VTX_PKCOLOR         (1 << 3)
#define VTX_ST0             (1 << 7)
#define VTX_Z               (1u << 31)
#define FMT_COLOR           (VTX_Z | VTX_PKCOLOR)
#define FMT_TEX             (VTX_Z | VTX_PKCOLOR | VTX_ST0)
#define PRIM_TRIS           4
#define PRIM_RECTS          8
#define WALK_IND            (1 << 4)
#define WALK_DATA           (3 << 4)

#define RED                 0xffff0000
#define GREEN               0xff00ff00
#define BLUE                0xff0000ff
#define BLACK               0xff000000

typedef struct {
    QTestState *qts;
    QPCIBus *bus;
    QPCIDevice *dev;
    QPCIBar fb, mmio;
} Ctx;

static uint32_t f2u(float f)
{
    uint32_t u;

    memcpy(&u, &f, 4);
    return u;
}

static void reg(Ctx *t, uint32_t r, uint32_t v)
{
    qpci_io_writel(t->dev, t->mmio, r, v);
}

static void pkt3(Ctx *t, int op, const uint32_t *d, int n)
{
    int i;

    reg(t, CSQ_APER, 0xc0000000 | (n - 1) << 16 | op << 8);
    for (i = 0; i < n; i++) {
        reg(t, CSQ_APER, d[i]);
    }
}

static uint32_t pixel(Ctx *t, int x, int y)
{
    return qpci_io_readl(t->dev, t->fb, CB + (y * W + x) * 4);
}

static void start(Ctx *t)
{
    t->qts = qtest_init("-vga none -device ati-vga,model=rv100,addr=04.0");
    t->bus = qpci_new_pc(t->qts, NULL);
    t->dev = qpci_device_find(t->bus, QPCI_DEVFN(4, 0));
    g_assert(t->dev);
    qpci_device_enable(t->dev);
    t->fb = qpci_iomap(t->dev, 0, NULL);
    t->mmio = qpci_iomap(t->dev, 2, NULL);

    /* the frame buffer at GPU address 0, 16 MB, no byte swapping */
    reg(t, MC_FB_LOCATION, 0x00ff0000);
    reg(t, SURFACE_CNTL, 0);
    reg(t, RB3D_COLOROFFSET, CB);
    reg(t, RB3D_COLORPITCH, W);
    reg(t, RB3D_DEPTHOFFSET, ZB);
    reg(t, RB3D_DEPTHPITCH, W);
    reg(t, RE_TOP_LEFT, 0);
    reg(t, RE_WIDTH_HEIGHT, (W - 1) | (W - 1) << 16);
    reg(t, SE_COORD_FMT, 3);     /* x, y and z are in window coordinates */
    reg(t, SE_CNTL, SE_SOLID);
    reg(t, PP_CNTL, 0);
    reg(t, PP_MISC, 0);
}

static void stop(Ctx *t)
{
    g_free(t->dev);
    qpci_free_pc(t->bus);
    qtest_quit(t->qts);
}

/* a rectangle from x0, y0 to x1, y1 at depth z, in one colour */
static void rect(Ctx *t, float x0, float y0, float x1, float y1, float z,
                 uint32_t argb)
{
    uint32_t d[2 + 3 * 4] = {
        FMT_COLOR, PRIM_RECTS | WALK_DATA | 3 << 16,
        f2u(x0), f2u(y0), f2u(z), argb,
        f2u(x1), f2u(y0), f2u(z), argb,
        f2u(x1), f2u(y1), f2u(z), argb,
    };

    pkt3(t, 0x29, d, ARRAY_SIZE(d));
}

static void tri(Ctx *t, const float *xy, float z, uint32_t argb)
{
    uint32_t d[2 + 3 * 4];
    int i;

    d[0] = FMT_COLOR;
    d[1] = PRIM_TRIS | WALK_DATA | 3 << 16;
    for (i = 0; i < 3; i++) {
        d[2 + i * 4] = f2u(xy[i * 2]);
        d[3 + i * 4] = f2u(xy[i * 2 + 1]);
        d[4 + i * 4] = f2u(z);
        d[5 + i * 4] = argb;
    }
    pkt3(t, 0x29, d, ARRAY_SIZE(d));
}

/* clears the colour to black and the depth to the far plane */
static void clear(Ctx *t, int depth_fmt)
{
    reg(t, RB3D_CNTL, RB_COLOR_ARGB8888 | RB_Z_ENABLE);
    reg(t, RB3D_ZSTENCILCNTL, depth_fmt | Z_TEST(FUNC_ALWAYS) | Z_WRITE);
    rect(t, 0, 0, W, W, 1.0f, BLACK);
}

/* a nearer rectangle hides a farther one drawn after it */
static void test_depth(void)
{
    Ctx t;

    start(&t);
    clear(&t, DEPTH_16);
    reg(&t, RB3D_ZSTENCILCNTL, DEPTH_16 | Z_TEST(FUNC_LESS) | Z_WRITE);
    rect(&t, 0, 0, 32, 64, 0.25f, RED);
    rect(&t, 0, 0, 64, 64, 0.5f, GREEN);
    rect(&t, 16, 0, 48, 64, 0.75f, BLUE);
    g_assert_cmphex(pixel(&t, 8, 8), ==, RED);
    g_assert_cmphex(pixel(&t, 40, 8), ==, GREEN);
    g_assert_cmphex(pixel(&t, 60, 60), ==, GREEN);
    stop(&t);
}

/* with back faces culled, the order of the vertices decides */
static void test_cull(void)
{
    /* clockwise on the screen (y goes down), then the other way round */
    static const float cw[6] = { 0, 0, 32, 0, 0, 32 };
    static const float ccw[6] = { 32, 32, 32, 63, 63, 32 };
    Ctx t;

    start(&t);
    clear(&t, DEPTH_16);
    reg(&t, RB3D_CNTL, RB_COLOR_ARGB8888);
    reg(&t, SE_CNTL, SE_CULL_BACK);
    tri(&t, cw, 0.5f, RED);
    tri(&t, ccw, 0.5f, GREEN);
    g_assert_cmphex(pixel(&t, 4, 4), ==, RED);
    g_assert_cmphex(pixel(&t, 36, 36), ==, BLACK);
    stop(&t);
}

/* a rectangle marks the stencil; only the marked pixels pass EQUAL */
static void test_stencil(void)
{
    Ctx t;

    start(&t);
    clear(&t, DEPTH_24_8);
    reg(&t, RB3D_CNTL, RB_COLOR_ARGB8888 | RB_Z_ENABLE | RB_STENCIL_ENABLE);
    reg(&t, RB3D_STENCILREFMASK, 1 | 0xff << 16 | 0xff << 24);
    reg(&t, RB3D_ZSTENCILCNTL, DEPTH_24_8 | Z_TEST(FUNC_ALWAYS) |
        ST_TEST(FUNC_ALWAYS) | ST_ZPASS(ST_REPLACE));
    rect(&t, 16, 16, 32, 32, 0.5f, RED);
    reg(&t, RB3D_ZSTENCILCNTL, DEPTH_24_8 | Z_TEST(FUNC_ALWAYS) |
        ST_TEST(FUNC_EQUAL));
    rect(&t, 0, 0, 64, 64, 0.5f, GREEN);
    g_assert_cmphex(pixel(&t, 20, 20), ==, GREEN);
    g_assert_cmphex(pixel(&t, 40, 40), ==, BLACK);
    g_assert_cmphex(pixel(&t, 4, 4), ==, BLACK);
    stop(&t);
}

/* 3D_RNDR_GEN_INDX_PRIM: two triangles picked by indices from a buffer */
static void test_indexed(void)
{
    static const float xy[4][2] = { { 8, 8 }, { 40, 8 }, { 40, 40 }, { 8, 40 } };
    uint32_t vb[4 * 4];
    uint32_t d[4 + 3];
    int i;
    Ctx t;

    start(&t);
    clear(&t, DEPTH_16);
    reg(&t, RB3D_CNTL, RB_COLOR_ARGB8888);
    for (i = 0; i < 4; i++) {
        vb[i * 4] = cpu_to_le32(f2u(xy[i][0]));
        vb[i * 4 + 1] = cpu_to_le32(f2u(xy[i][1]));
        vb[i * 4 + 2] = cpu_to_le32(f2u(0.5f));
        vb[i * 4 + 3] = cpu_to_le32(BLUE);
    }
    qpci_memwrite(t.dev, t.fb, VB, vb, sizeof(vb));
    d[0] = VB;
    d[1] = 4;
    d[2] = FMT_COLOR;
    d[3] = PRIM_TRIS | WALK_IND | 6 << 16;
    d[4] = 0 | 1 << 16;
    d[5] = 2 | 0 << 16;
    d[6] = 2 | 3 << 16;
    pkt3(&t, 0x23, d, ARRAY_SIZE(d));
    g_assert_cmphex(pixel(&t, 12, 36), ==, BLUE);
    g_assert_cmphex(pixel(&t, 36, 12), ==, BLUE);
    g_assert_cmphex(pixel(&t, 50, 50), ==, BLACK);
    stop(&t);
}

/* a textured rectangle from s, t 0-1 over w x w pixels */
static void tex_rect(Ctx *t, float x, float y, float w)
{
    uint32_t d[2 + 3 * 6] = {
        FMT_TEX, PRIM_RECTS | WALK_DATA | 3 << 16,
        f2u(x), f2u(y), f2u(0.5f), 0xffffffff, f2u(0), f2u(0),
        f2u(x + w), f2u(y), f2u(0.5f), 0xffffffff, f2u(1), f2u(0),
        f2u(x + w), f2u(y + w), f2u(0.5f), 0xffffffff, f2u(1), f2u(1),
    };

    pkt3(t, 0x29, d, ARRAY_SIZE(d));
}

/* texture 0 replaces the colour, nearest texel of the nearest level */
static void tex_state(Ctx *t, int levels_log2)
{
    reg(t, RB3D_CNTL, RB_COLOR_ARGB8888);
    reg(t, PP_CNTL, 1 << 4 | 1 << 12);
    reg(t, PP_TXCBLEND_0, 10 | 1 << 16 | 1 << 23);   /* T0 x 1 + 0 */
    reg(t, PP_TXABLEND_0, 5 | 1 << 16 | 1 << 23);
    reg(t, PP_TXFILTER_0, 2 << 1 | levels_log2 << 16 | 2 << 23 | 2 << 27);
    reg(t, PP_TXFORMAT_0, 6 | 0x40 | levels_log2 << 8 | levels_log2 << 12);
    reg(t, PP_TXOFFSET_0, TEX);
}

/* the levels of a 4 x 4 texture follow each other, rows 32 byte aligned */
static void test_mipmap(void)
{
    static const uint32_t colour[3] = { RED, GREEN, BLUE };
    static const int off[3] = { 0, 4 * 32, 4 * 32 + 2 * 32 };
    int l, i;
    Ctx t;

    start(&t);
    clear(&t, DEPTH_16);
    for (l = 0; l < 3; l++) {
        int side = 4 >> l;

        for (i = 0; i < side * side; i++) {
            qpci_io_writel(t.dev, t.fb, TEX + off[l] + (i / side) * 32 +
                           (i % side) * 4, colour[l]);
        }
    }
    tex_state(&t, 2);
    tex_rect(&t, 0, 0, 4);       /* a texel a pixel: level 0 */
    tex_rect(&t, 8, 0, 2);       /* two texels a pixel: level 1 */
    tex_rect(&t, 16, 0, 1);      /* four texels a pixel: level 2 */
    g_assert_cmphex(pixel(&t, 1, 1), ==, RED);
    g_assert_cmphex(pixel(&t, 8, 0), ==, GREEN);
    g_assert_cmphex(pixel(&t, 16, 0), ==, BLUE);
    stop(&t);
}

/*
 * A texel written through a surface that swaps 32 bit words: the engine
 * sees it little endian once the surface is gone.
 */
static void test_surface_swap(void)
{
    static const uint8_t be_green[4] = { 0xff, 0x00, 0xff, 0x00 };
    uint8_t raw[4];
    Ctx t;

    start(&t);
    clear(&t, DEPTH_16);
    reg(&t, SURFACE0_LOWER_BOUND, TEX);
    reg(&t, SURFACE0_UPPER_BOUND, TEX + 0xfff);
    reg(&t, SURFACE0_INFO, 1 << 21);     /* aperture 0 swaps 32 bit words */
    qpci_memwrite(t.dev, t.fb, TEX, be_green, 4);
    reg(&t, SURFACE0_UPPER_BOUND, 0);    /* the surface is off */
    qpci_memread(t.dev, t.fb, TEX, raw, 4);
    g_assert_cmphex((uint32_t)ldl_le_p(raw), ==, GREEN);
    tex_state(&t, 0);
    tex_rect(&t, 0, 0, 4);
    g_assert_cmphex(pixel(&t, 2, 2), ==, GREEN);
    stop(&t);
}

/*
 * A texel written while SURFACE_CNTL swaps 32 bit words outside the
 * surfaces: the engine still sees it little endian after the swap is off.
 */
static void test_nonsurf_swap(void)
{
    static const uint8_t be_red[4] = { 0xff, 0xff, 0x00, 0x00 };
    uint8_t raw[4];
    Ctx t;

    start(&t);
    clear(&t, DEPTH_16);
    reg(&t, SURFACE_CNTL, 1 << 21);      /* aperture 0 swaps 32 bit words */
    qpci_memwrite(t.dev, t.fb, TEX, be_red, 4);
    reg(&t, SURFACE_CNTL, 0);
    qpci_memread(t.dev, t.fb, TEX, raw, 4);
    g_assert_cmphex((uint32_t)ldl_le_p(raw), ==, RED);
    tex_state(&t, 0);
    tex_rect(&t, 0, 0, 4);
    g_assert_cmphex(pixel(&t, 2, 2), ==, RED);
    stop(&t);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    if (!qtest_has_device("ati-vga")) {
        return g_test_run();
    }
    qtest_add_func("/ati-3d/depth", test_depth);
    qtest_add_func("/ati-3d/cull", test_cull);
    qtest_add_func("/ati-3d/stencil", test_stencil);
    qtest_add_func("/ati-3d/indexed", test_indexed);
    qtest_add_func("/ati-3d/mipmap", test_mipmap);
    qtest_add_func("/ati-3d/surface-swap", test_surface_swap);
    qtest_add_func("/ati-3d/nonsurf-swap", test_nonsurf_swap);
    return g_test_run();
}
