/*
 * QTest testcase for the PowerPC 4xx EMAC and MAL on the sam440ep
 *
 * Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"

/*
 * The MAL is set up through DCRs, which qtest cannot reach, so the test
 * makes a small firmware that does it: it sets the channel table pointers,
 * the receive buffer size and the active channels, maps RAM, writes a mark
 * to RAM and spins. The test then puts the descriptors and a frame in RAM
 * and sends the frame from EMAC0 to EMAC1 through a hub.
 */

#define FW_SIZE         (512 * 1024)    /* 0xfff80000-0xffffffff */

#define EMAC0           0xef600e00
#define EMAC1           0xef600f00
#define EMAC_MR0        0x00
#define EMAC_TMR0       0x08
#define EMAC_RMR        0x10
#define EMAC_STACR      0x5c
#define EMAC_OCTX       0x68
#define EMAC_OCRX       0x6c

#define MR0_TXE         0x10000000
#define MR0_RXE         0x08000000
#define TMR0_GNP0       0x80000000
#define RMR_SFCS        0x40000000
#define RMR_BAE         0x00100000
#define STACR_OC        0x00008000
#define STACR_READ      0x00001000

#define MAL0_TXCASR     0x184
#define MAL0_RXCASR     0x190
#define MAL0_TXCTP0R    0x1a0
#define MAL0_RXCTP0R    0x1c0
#define MAL0_RCBS0      0x1e0

/* EMAC0 sends on MAL channel 0, EMAC1 receives on channel 1 */
#define TX_CHAN         0
#define RX_CHAN         1

#define TX_DESC         0x00100000
#define RX_DESC         0x00100010
#define MARK            0x00100100
#define TX_BUF          0x00101000
#define RX_BUF          0x00102000
#define MARK_VALUE      0x600dcafe

#define MAL_READY       0x8000      /* TX: ready, RX: empty */
#define MAL_WRAP        0x4000
#define MAL_LAST        0x1000
#define MAL_FIRST       0x0800

#define FRAME_LEN       64

/* PowerPC instructions */
static uint32_t lis(int rd, uint16_t imm)
{
    return 0x3c000000 | rd << 21 | imm;
}

static uint32_t ori(int ra, int rs, uint16_t imm)
{
    return 0x60000000 | rs << 21 | ra << 16 | imm;
}

static uint32_t mtdcr(int dcrn, int rs)
{
    return 0x7c000386 | rs << 21 | (dcrn & 0x1f) << 16 |
           ((dcrn >> 5) & 0x1f) << 11;
}

static uint32_t tlbwe(int rs, int ra, int ws)
{
    return 0x7c0007a4 | rs << 21 | ra << 16 | ws << 11;
}

static uint32_t stw(int rs, int ra, uint16_t d)
{
    return 0x90000000 | rs << 21 | ra << 16 | d;
}

#define ISYNC           0x4c00012c
#define B_SELF          0x48000000

typedef struct {
    uint8_t *fw;
    int pc;
} Fw;

static void emit(Fw *f, uint32_t insn)
{
    stl_be_p(f->fw + f->pc, insn);
    f->pc += 4;
}

static void emit_li32(Fw *f, int rd, uint32_t val)
{
    emit(f, lis(rd, val >> 16));
    emit(f, ori(rd, rd, val & 0xffff));
}

static void emit_dcr(Fw *f, int dcrn, uint32_t val)
{
    emit_li32(f, 3, val);
    emit(f, mtdcr(dcrn, 3));
}

static char *make_firmware(void)
{
    Fw f = { .fw = g_malloc0(FW_SIZE) };
    char *path;
    int fd;

    g_assert((fd = g_file_open_tmp("emac-fw-XXXXXX", &path, NULL)) >= 0);

    emit_dcr(&f, MAL0_TXCTP0R + TX_CHAN, TX_DESC);
    emit_dcr(&f, MAL0_RXCTP0R + RX_CHAN, RX_DESC);
    emit_dcr(&f, MAL0_RCBS0 + RX_CHAN, 256 / 16);
    emit_dcr(&f, MAL0_TXCASR, 0x80000000u >> TX_CHAN);
    emit_dcr(&f, MAL0_RXCASR, 0x80000000u >> RX_CHAN);

    /* TLB entry 1: the first 256 MB of RAM, supervisor read and write */
    emit_li32(&f, 4, 1);
    emit_li32(&f, 3, 0x00000000 | 0x200 | 0x90);  /* EPN 0, valid, 256 MB */
    emit(&f, tlbwe(3, 4, 0));
    emit_li32(&f, 3, 0);                            /* RPN 0 */
    emit(&f, tlbwe(3, 4, 1));
    emit_li32(&f, 3, 0x3f);                         /* all permissions */
    emit(&f, tlbwe(3, 4, 2));
    emit(&f, ISYNC);

    emit_li32(&f, 3, MARK_VALUE);
    emit_li32(&f, 4, MARK);
    emit(&f, stw(3, 4, 0));
    emit(&f, B_SELF);

    /* the reset vector at 0xfffffffc branches to the start, 0xfff80000 */
    stl_be_p(f.fw + FW_SIZE - 4, 0x48000000 | ((-(FW_SIZE - 4)) & 0x03fffffc));

    g_assert(write(fd, f.fw, FW_SIZE) == FW_SIZE);
    close(fd);
    g_free(f.fw);
    return path;
}

static QTestState *emac_start(char **fw)
{
    QTestState *qts;

    *fw = make_firmware();
    qts = qtest_initf("-M sam440ep -nodefaults -accel tcg -bios %s "
                      "-netdev hubport,id=n0,hubid=0 "
                      "-netdev hubport,id=n1,hubid=0 "
                      "-net nic,model=ppc4xx-emac,netdev=n0 "
                      "-net nic,model=ppc4xx-emac,netdev=n1", *fw);
    return qts;
}

static void emac_stop(QTestState *qts, char *fw)
{
    qtest_quit(qts);
    unlink(fw);
    g_free(fw);
}

static uint16_t mdio_read(QTestState *qts, uint32_t emac, int phy, int reg)
{
    uint32_t v;

    qtest_writel(qts, emac + EMAC_STACR, STACR_READ | phy << 5 | reg);
    v = qtest_readl(qts, emac + EMAC_STACR);
    g_assert(v & STACR_OC);
    return v >> 16;
}

/* each EMAC reaches its own PHY: 24 for EMAC0, 25 for EMAC1 */
static void test_mdio(void)
{
    char *fw;
    QTestState *qts = emac_start(&fw);

    g_assert_cmphex(mdio_read(qts, EMAC0, 24, 2), ==, 0x0181);
    g_assert_cmphex(mdio_read(qts, EMAC0, 24, 3), ==, 0xb8a0);
    g_assert(mdio_read(qts, EMAC0, 24, 1) & 0x0004);      /* link up */
    g_assert_cmphex(mdio_read(qts, EMAC0, 25, 2), ==, 0xffff);
    g_assert_cmphex(mdio_read(qts, EMAC1, 25, 2), ==, 0x0181);
    emac_stop(qts, fw);
}

static void wd(QTestState *qts, uint64_t addr, uint16_t ctrl, uint16_t len,
               uint32_t buf)
{
    qtest_writew(qts, addr, ctrl);
    qtest_writew(qts, addr + 2, len);
    qtest_writel(qts, addr + 4, buf);
}

/* a broadcast frame goes out of EMAC0 and lands in EMAC1's buffer */
static void test_loopback(void)
{
    char *fw;
    QTestState *qts = emac_start(&fw);
    uint8_t frame[FRAME_LEN], got[FRAME_LEN];
    int i;

    for (i = 0; i < 200 && qtest_readl(qts, MARK) != MARK_VALUE; i++) {
        g_usleep(10 * 1000);
    }
    g_assert_cmphex(qtest_readl(qts, MARK), ==, MARK_VALUE);

    memset(frame, 0xff, 6);
    memcpy(frame + 6, "\x52\x54\x00\x12\x34\x56", 6);
    frame[12] = 0x08;
    frame[13] = 0x00;
    for (i = 14; i < FRAME_LEN; i++) {
        frame[i] = i;
    }
    qtest_memwrite(qts, TX_BUF, frame, sizeof(frame));
    wd(qts, TX_DESC, MAL_READY | MAL_WRAP | MAL_LAST, FRAME_LEN, TX_BUF);
    wd(qts, RX_DESC, MAL_READY | MAL_WRAP, 0, RX_BUF);

    qtest_writel(qts, EMAC1 + EMAC_RMR, RMR_BAE | RMR_SFCS);
    qtest_writel(qts, EMAC1 + EMAC_MR0, MR0_RXE);
    qtest_writel(qts, EMAC0 + EMAC_MR0, MR0_TXE);
    qtest_writel(qts, EMAC0 + EMAC_TMR0, TMR0_GNP0);

    /* sent: ready is clear */
    g_assert_cmphex(qtest_readw(qts, TX_DESC), ==, MAL_WRAP | MAL_LAST);
    /* received in one buffer: not empty, first and last */
    g_assert_cmphex(qtest_readw(qts, RX_DESC), ==,
                    MAL_WRAP | MAL_FIRST | MAL_LAST);
    g_assert_cmpuint(qtest_readw(qts, RX_DESC + 2), ==, FRAME_LEN);
    qtest_memread(qts, RX_BUF, got, sizeof(got));
    g_assert(!memcmp(frame, got, sizeof(got)));
    g_assert_cmpuint(qtest_readl(qts, EMAC0 + EMAC_OCTX), ==, FRAME_LEN);
    g_assert_cmpuint(qtest_readl(qts, EMAC1 + EMAC_OCRX), ==, FRAME_LEN);
    emac_stop(qts, fw);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    if (!qtest_has_machine("sam440ep")) {
        return g_test_run();
    }
    qtest_add_func("/ppc4xx-emac/mdio", test_mdio);
    qtest_add_func("/ppc4xx-emac/loopback", test_loopback);
    return g_test_run();
}
