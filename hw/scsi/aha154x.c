/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Adaptec AHA-154xB ISA SCSI host adapter.
 *
 * The host talks to the board through three I/O ports and 24-bit
 * mailboxes in its memory; the board is an ISA bus master. The board's
 * BIOS (an option ROM such as aha1540b320_330.bin, given with romfile=)
 * reads disks through the "start BIOS command" (03h).
 *
 * The behaviour of the ports, the commands, the CCBs and the BIOS command
 * follows 86Box's src/scsi/scsi_x54x.c and src/scsi/scsi_aha154x.c:
 * Copyright 2016-2018 Miran Grca.
 * Copyright 2017-2018 Fred N. van Kempen.
 * Reference: Adaptec AHA-1540B/1542B Technical Reference Manual.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/main-loop.h"
#include "qapi/error.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/loader.h"
#include "hw/core/irq.h"
#include "hw/isa/isa.h"
#include "hw/scsi/scsi.h"
#include "scsi/constants.h"
#include "system/dma.h"
#include "migration/vmstate.h"
#include "trace.h"
#include "qom/object.h"

#define TYPE_AHA154X "aha154x"
OBJECT_DECLARE_SIMPLE_TYPE(AHA154xState, AHA154X)

/* Control port (write) */
#define CTRL_HRST   0x80
#define CTRL_SRST   0x40
#define CTRL_IRST   0x20
#define CTRL_SCRST  0x10

/* Status port (read) */
#define STAT_STST   0x80
#define STAT_DFAIL  0x40
#define STAT_INIT   0x20
#define STAT_IDLE   0x10
#define STAT_CDF    0x08
#define STAT_DF     0x04
#define STAT_INVCMD 0x01

/* Interrupt flags port (read) */
#define INTR_ANY    0x80
#define INTR_SCRD   0x08
#define INTR_HACC   0x04
#define INTR_MBOA   0x02
#define INTR_MBIF   0x01

/* Commands */
#define CMD_NOP         0x00
#define CMD_MBINIT      0x01
#define CMD_START_SCSI  0x02
#define CMD_BIOSCMD     0x03
#define CMD_INQUIRY     0x04
#define CMD_EMBOI       0x05
#define CMD_SELTIMEOUT  0x06
#define CMD_BUSON_TIME  0x07
#define CMD_BUSOFF_TIME 0x08
#define CMD_DMASPEED    0x09
#define CMD_RETDEVS     0x0a
#define CMD_RETCONF     0x0b
#define CMD_RETSETUP    0x0d
#define CMD_WRITE_CH2   0x1a
#define CMD_READ_CH2    0x1b
#define CMD_ECHO        0x1f
#define CMD_OPTIONS     0x21
#define CMD_EXTBIOS     0x28
#define CMD_MBENABLE    0x29

/* Mailbox out action codes, mailbox in completion codes */
#define MBO_FREE        0x00
#define MBO_START       0x01
#define MBO_ABORT       0x02
#define MBI_SUCCESS     0x01
#define MBI_ABORT       0x02
#define MBI_NOT_FOUND   0x03
#define MBI_ERROR       0x04

/* CCB operation codes */
#define CCB_INITIATOR       0x00
#define CCB_SG              0x02
#define CCB_INITIATOR_RES   0x03
#define CCB_SG_RES          0x04
#define CCB_BUS_DEV_RESET   0x81

/* CCB host status */
#define HS_OK               0x00
#define HS_SEL_TIMEOUT      0x11
#define HS_INVALID_OPCODE   0x16
#define HS_ABORT_NOT_FOUND  0x26

#define CCB_HDR_LEN     18
#define DATABUF_LEN     256

typedef struct AHA154xState AHA154xState;

typedef struct AHARequest {
    AHA154xState *s;
    SCSIRequest *sreq;
    QEMUSGList sg;
    bool has_sg;
    bool bios;              /* the BIOS command (03h), not a CCB */
    bool aborted;           /* the host aborted the CCB */
    uint32_t ccb;
    uint8_t opcode;
    uint8_t cdblen;
    uint8_t senselen;
    /* data of a BIOS command without a buffer in guest memory */
    uint8_t buf[8];
    uint32_t buflen;
    QTAILQ_ENTRY(AHARequest) next;
} AHARequest;

struct AHA154xState {
    ISADevice parent_obj;

    MemoryRegion io;
    qemu_irq irq;
    SCSIBus bus;
    QEMUBH *mbo_bh;
    QEMUTimer *reset_timer;

    uint32_t iobase;
    uint32_t isairq;
    uint32_t isadma;
    uint8_t host_id;
    char *romfile;
    char *fwrev;
    char fw[4];

    uint8_t status;
    uint8_t intr;
    uint8_t pending_intr;
    uint8_t cmd;                /* 0xff: none */
    uint8_t cmdbuf[64];
    int cmdparam;
    int cmdleft;
    uint8_t databuf[DATABUF_LEN];
    int datapos;
    int dataleft;
    bool bios_busy;

    bool mbo_intr;
    uint32_t mb_count;
    uint32_t mbo_addr;
    uint32_t mbi_addr;
    uint32_t mbo_pos;
    uint32_t mbi_pos;
    uint8_t bus_on;
    uint8_t bus_off;
    uint8_t dma_speed;
    uint8_t ch2buf[64];
    uint8_t bios_sense[8][14];
    bool resetting;
    uint32_t tag;

    QTAILQ_HEAD(, AHARequest) reqs;
};

static AddressSpace *aha_as(void)
{
    return &address_space_memory;
}

static void aha_read(uint32_t addr, void *buf, int len)
{
    dma_memory_read(aha_as(), addr & 0xffffff, buf, len,
                    MEMTXATTRS_UNSPECIFIED);
}

static void aha_write(uint32_t addr, const void *buf, int len)
{
    dma_memory_write(aha_as(), addr & 0xffffff, buf, len,
                     MEMTXATTRS_UNSPECIFIED);
}

static uint32_t get24(const uint8_t *p)
{
    return (p[0] << 16) | (p[1] << 8) | p[2];
}

static void put24(uint8_t *p, uint32_t v)
{
    p[0] = v >> 16;
    p[1] = v >> 8;
    p[2] = v;
}

static void aha_update_irq(AHA154xState *s, bool raise)
{
    qemu_set_irq(s->irq, raise);
}

static void aha_raise(AHA154xState *s, bool suppress, uint8_t bits)
{
    if (bits & (INTR_MBIF | INTR_MBOA)) {
        if (!(s->intr & INTR_HACC)) {
            s->intr |= bits;
        } else {
            s->pending_intr |= bits;
        }
    } else {
        s->intr |= bits;
    }
    s->intr |= INTR_ANY;
    trace_aha154x_irq(s->intr, s->pending_intr, !suppress);
    if (!suppress) {
        aha_update_irq(s, true);
    }
}

static void aha_clear_irq(AHA154xState *s)
{
    s->intr = 0;
    aha_update_irq(s, false);
    if (s->pending_intr) {
        uint8_t p = s->pending_intr;

        s->pending_intr = 0;
        aha_raise(s, false, p);
    }
}

static void aha_cmd_done(AHA154xState *s, bool suppress)
{
    s->datapos = 0;
    s->dataleft = 0;
    s->status |= STAT_IDLE;
    s->status &= ~STAT_DF;
    aha_raise(s, suppress, INTR_HACC);
    s->cmd = 0xff;
    s->cmdparam = 0;
    s->cmdleft = 0;
}

static void aha_free_req(AHARequest *r)
{
    AHA154xState *s = r->s;

    QTAILQ_REMOVE(&s->reqs, r, next);
    if (r->has_sg) {
        qemu_sglist_destroy(&r->sg);
    }
    g_free(r);
}

/* Cancel everything in flight; no mailbox in for these. */
static void aha_cancel_all(AHA154xState *s)
{
    AHARequest *r, *nr;

    s->resetting = true;
    QTAILQ_FOREACH_SAFE(r, &s->reqs, next, nr) {
        if (r->sreq) {
            scsi_req_cancel(r->sreq);
        } else {
            aha_free_req(r);
        }
    }
    s->resetting = false;
    s->bios_busy = false;
}

static void aha_reset_state(AHA154xState *s)
{
    aha_cancel_all(s);
    s->intr = 0;
    s->pending_intr = 0;
    aha_update_irq(s, false);
    s->cmd = 0xff;
    s->cmdparam = 0;
    s->cmdleft = 0;
    s->datapos = 0;
    s->dataleft = 0;
    s->mbo_intr = false;
    s->mb_count = 0;
    s->mbo_pos = 0;
    s->mbi_pos = 0;
}

static void aha_reset_done(void *opaque)
{
    AHA154xState *s = opaque;

    s->status = STAT_INIT | STAT_IDLE;
}

static void aha_reset_ctrl(AHA154xState *s, bool hard)
{
    trace_aha154x_reset(hard);
    aha_reset_state(s);
    if (hard) {
        /* Self test, as the board does after a hard reset. */
        s->status = STAT_STST;
        timer_mod(s->reset_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 2 * SCALE_MS);
    } else {
        timer_del(s->reset_timer);
        s->status = STAT_INIT | STAT_IDLE;
    }
}

/* Mailbox in: return a CCB to the host. */
static void aha_mbi(AHA154xState *s, uint32_t ccb, uint8_t code,
                    uint8_t hs, uint8_t ts)
{
    uint8_t mb[4];

    if (code != MBI_NOT_FOUND) {
        uint8_t st[2] = { hs, ts };

        aha_write(ccb + 14, st, 2);
    }
    if (!s->mb_count) {
        return;
    }
    mb[0] = code;
    put24(mb + 1, ccb);
    aha_write(s->mbi_addr + s->mbi_pos * 4, mb, 4);
    trace_aha154x_mbi(s->mbi_pos, ccb, code, hs, ts);
    s->mbi_pos = (s->mbi_pos + 1) % s->mb_count;
    aha_raise(s, false, INTR_MBIF | (s->mbo_intr ? INTR_MBOA : 0));
}

/* BIOS command completion code from the sense data, as 86Box does. */
static uint8_t aha_bios_code(const uint8_t *sense)
{
    switch (sense[12]) {
    case 0x00:
        return 0x00;
    case 0x20: case 0x24: case 0x26: case 0x4b:
        return 0x01;
    case 0x12: case 0x21:
        return 0x02;
    case 0x27:
        return 0x03;
    case 0x14: case 0x16:
        return 0x04;
    case 0x30: case 0x64:
        return 0x0c;
    case 0x10: case 0x11:
        return 0x10;
    case 0x17: case 0x18:
        return 0x11;
    case 0x01: case 0x03: case 0x05: case 0x06: case 0x07: case 0x08:
    case 0x09: case 0x1b: case 0x1c: case 0x1d:
    case 0x40 ... 0x49:
        return 0x20;
    case 0x15: case 0x02:
        return 0x40;
    case 0x25:
        return 0x80;
    case 0x04: case 0x28: case 0x29: case 0x2a: case 0x3a:
        return 0xaa;
    default:
        return 0xff;
    }
}

static void aha_bios_finish(AHA154xState *s, uint8_t code)
{
    s->bios_busy = false;
    s->databuf[0] = code;
    s->datapos = 0;
    s->dataleft = 1;
    s->status |= STAT_DF;
    trace_aha154x_bios_done(code);
}

static void aha_bios_complete(AHARequest *r, SCSIRequest *req)
{
    AHA154xState *s = r->s;
    uint8_t sense[SCSI_SENSE_BUF_SIZE] = { 0 };
    uint8_t code = 0;
    int id = req->dev->id & 7;

    if (req->status != GOOD) {
        scsi_req_get_sense(req, sense, sizeof(sense));
        code = aha_bios_code(sense);
        if (!code) {
            code = 0xff;
        }
    }
    memcpy(s->bios_sense[id], sense, 14);
    if (code == 0 && r->buflen) {
        /* Read drive parameters: the last LBA, big endian. */
        aha_write(get24(&s->cmdbuf[7]), r->buf, 4);
    }
    if (r->opcode == 0x0c) {
        code = !!code;
    }
    aha_bios_finish(s, code);
}

static void aha_ccb_complete(AHARequest *r, SCSIRequest *req, size_t resid)
{
    AHA154xState *s = r->s;
    uint8_t ts = req->status;

    if (req->status == CHECK_CONDITION && r->senselen != 1) {
        uint8_t sense[SCSI_SENSE_BUF_SIZE] = { 0 };
        int len = r->senselen ? r->senselen : 14;

        scsi_req_get_sense(req, sense, sizeof(sense));
        aha_write(r->ccb + CCB_HDR_LEN + r->cdblen, sense,
                  MIN(len, (int)sizeof(sense)));
    }
    if (r->opcode == CCB_INITIATOR_RES || r->opcode == CCB_SG_RES) {
        uint8_t res[3];

        put24(res, resid);
        aha_write(r->ccb + 4, res, 3);
    }
    aha_mbi(s, r->ccb, ts == GOOD ? MBI_SUCCESS : MBI_ERROR, HS_OK, ts);
}

static void aha_complete(SCSIRequest *req, size_t resid)
{
    AHARequest *r = req->hba_private;

    trace_aha154x_complete(r->ccb, req->status, resid);
    if (r->bios) {
        aha_bios_complete(r, req);
    } else {
        aha_ccb_complete(r, req, resid);
    }
    scsi_req_unref(req);
    r->sreq = NULL;
    aha_free_req(r);
}

static void aha_cancel(SCSIRequest *req)
{
    AHARequest *r = req->hba_private;
    AHA154xState *s = r->s;

    if (!s->resetting) {
        if (r->bios) {
            aha_bios_finish(s, 0xff);
        } else if (r->aborted) {
            aha_mbi(s, r->ccb, MBI_ABORT, HS_OK, GOOD);
        }
    }
    scsi_req_unref(req);
    r->sreq = NULL;
    aha_free_req(r);
}

static void aha_transfer_data(SCSIRequest *req, uint32_t len)
{
    AHARequest *r = req->hba_private;
    uint8_t *buf = scsi_req_get_buf(req);

    if (req->cmd.mode == SCSI_XFER_FROM_DEV) {
        r->buflen = MIN(len, sizeof(r->buf));
        memcpy(r->buf, buf, r->buflen);
    }
    scsi_req_continue(req);
}

static QEMUSGList *aha_get_sg_list(SCSIRequest *req)
{
    AHARequest *r = req->hba_private;

    return r->has_sg ? &r->sg : NULL;
}

static AHARequest *aha_new_req(AHA154xState *s)
{
    AHARequest *r = g_new0(AHARequest, 1);

    r->s = s;
    QTAILQ_INSERT_TAIL(&s->reqs, r, next);
    return r;
}

static void aha_submit(AHARequest *r, SCSIDevice *d, int lun,
                       uint8_t *cdb)
{
    int32_t n;

    r->sreq = scsi_req_new(d, ++r->s->tag, lun, cdb, 16, r);
    n = scsi_req_enqueue(r->sreq);
    if (n) {
        scsi_req_continue(r->sreq);
    }
}

static SCSIDevice *aha_find(AHA154xState *s, int id, int lun)
{
    SCSIDevice *d = scsi_device_find(&s->bus, 0, id, lun);

    if (!d || d->id != id || d->lun != lun) {
        return NULL;
    }
    return d;
}

/* Start BIOS command (03h): read, write and test the disks for the BIOS. */
static void aha_bios_cmd(AHA154xState *s)
{
    static const uint8_t to_scsi[0x12] = {
        [0x02] = READ_10, [0x03] = WRITE_10, [0x04] = VERIFY_10,
        [0x07] = FORMAT_UNIT, [0x0c] = SEEK_10,
        [0x10] = TEST_UNIT_READY, [0x11] = REZERO_UNIT,
    };
    uint8_t *c = s->cmdbuf;
    uint8_t op = c[0];
    int id = c[1] >> 5;
    int lun = c[1] & 7;
    uint32_t cyl = (c[2] << 8) | c[3];
    uint32_t lba = (cyl << 9) + ((c[4] & 0x0f) << 5) + (c[5] & 0x1f);
    uint32_t count = c[6];
    uint32_t addr = get24(&c[7]);
    uint8_t cdb[16] = { 0 };
    SCSIDevice *d;
    AHARequest *r;

    trace_aha154x_bios_cmd(op, id, lba, count, addr);
    s->bios_busy = true;

    d = lun ? NULL : aha_find(s, id, 0);
    if (!d || d->type == TYPE_ROM) {
        aha_bios_finish(s, 0x80);
        return;
    }

    switch (op) {
    case 0x00:
    case 0x06: case 0x09: case 0x0d: case 0x0e: case 0x0f: case 0x14:
        aha_bios_finish(s, 0x00);
        return;
    case 0x01:
        if (count) {
            aha_write(addr, s->bios_sense[id], 14);
        }
        aha_bios_finish(s, 0x00);
        return;
    case 0x02: case 0x03: case 0x04: case 0x0c:
        cdb[0] = to_scsi[op];
        stl_be_p(&cdb[2], lba);
        if (op != 0x0c) {
            cdb[8] = count;
        }
        break;
    case 0x07: case 0x10: case 0x11:
        cdb[0] = to_scsi[op];
        break;
    case 0x08: case 0x15:
        cdb[0] = READ_CAPACITY_10;
        break;
    default:
        aha_bios_finish(s, 0x01);
        return;
    }

    r = aha_new_req(s);
    r->bios = true;
    r->opcode = op;
    if (op == 0x02 || op == 0x03) {
        qemu_sglist_init(&r->sg, DEVICE(s), 1, aha_as());
        qemu_sglist_add(&r->sg, addr, count * 512);
        r->has_sg = true;
    }
    aha_submit(r, d, 0, cdb);
}

static void aha_start_ccb(AHA154xState *s, uint32_t ccb)
{
    uint8_t c[CCB_HDR_LEN + 16] = { 0 };
    uint8_t cdb[16] = { 0 };
    uint8_t opcode;
    int id, lun, dir;
    uint32_t len, ptr;
    SCSIDevice *d;
    AHARequest *r;

    aha_read(ccb, c, CCB_HDR_LEN + 12);
    opcode = c[0];
    id = c[1] >> 5;
    dir = (c[1] >> 3) & 3;
    lun = c[1] & 7;
    len = get24(&c[4]);
    ptr = get24(&c[7]);
    memcpy(cdb, &c[CCB_HDR_LEN], MIN(c[2], 12));
    trace_aha154x_ccb(ccb, opcode, id, lun, dir, cdb[0], len, ptr);

    if (opcode != CCB_INITIATOR && opcode != CCB_SG &&
        opcode != CCB_INITIATOR_RES && opcode != CCB_SG_RES &&
        opcode != CCB_BUS_DEV_RESET) {
        aha_mbi(s, ccb, MBI_ERROR, HS_INVALID_OPCODE, GOOD);
        return;
    }
    d = aha_find(s, id, lun);
    if (!d) {
        aha_mbi(s, ccb, MBI_ERROR, HS_SEL_TIMEOUT, GOOD);
        return;
    }
    if (opcode == CCB_BUS_DEV_RESET) {
        device_cold_reset(DEVICE(d));
        aha_mbi(s, ccb, MBI_SUCCESS, HS_OK, GOOD);
        return;
    }

    r = aha_new_req(s);
    r->ccb = ccb;
    r->opcode = opcode;
    r->cdblen = c[2];
    r->senselen = c[3];
    qemu_sglist_init(&r->sg, DEVICE(s), 1, aha_as());
    r->has_sg = true;
    if (dir != 3 && len) {
        if (opcode == CCB_SG || opcode == CCB_SG_RES) {
            uint32_t i;

            for (i = 0; i + 6 <= len; i += 6) {
                uint8_t sge[6];
                uint32_t sl;

                aha_read(ptr + i, sge, 6);
                sl = get24(&sge[0]);
                if (sl) {
                    qemu_sglist_add(&r->sg, get24(&sge[3]), sl);
                }
            }
        } else {
            qemu_sglist_add(&r->sg, ptr, len);
        }
    }
    aha_submit(r, d, lun, cdb);
}

static void aha_abort_ccb(AHA154xState *s, uint32_t ccb)
{
    AHARequest *r;

    QTAILQ_FOREACH(r, &s->reqs, next) {
        if (!r->bios && r->ccb == ccb && r->sreq) {
            r->aborted = true;
            scsi_req_cancel(r->sreq);
            return;
        }
    }
    aha_mbi(s, ccb, MBI_NOT_FOUND, HS_ABORT_NOT_FOUND, GOOD);
}

/* Look at every mailbox out once, from where the last scan stopped. */
static void aha_mbo_scan(void *opaque)
{
    AHA154xState *s = opaque;
    uint32_t i;

    if (!s->mb_count || (s->status & STAT_INIT)) {
        return;
    }
    for (i = 0; i < s->mb_count; i++) {
        uint32_t addr = s->mbo_addr + s->mbo_pos * 4;
        uint8_t mb[4];

        aha_read(addr, mb, 4);
        s->mbo_pos = (s->mbo_pos + 1) % s->mb_count;
        if (mb[0] == MBO_START || mb[0] == MBO_ABORT) {
            uint8_t free = MBO_FREE;

            aha_write(addr, &free, 1);
            if (mb[0] == MBO_START) {
                aha_start_ccb(s, get24(&mb[1]));
            } else {
                aha_abort_ccb(s, get24(&mb[1]));
            }
        }
    }
}

static int aha_param_len(uint8_t cmd)
{
    switch (cmd) {
    case CMD_MBINIT:
        return 4;
    case CMD_BIOSCMD:
        return 10;
    case CMD_EMBOI:
    case CMD_BUSON_TIME:
    case CMD_BUSOFF_TIME:
    case CMD_DMASPEED:
    case CMD_RETSETUP:
    case CMD_ECHO:
    case CMD_OPTIONS:
        return 1;
    case CMD_SELTIMEOUT:
        return 4;
    case CMD_WRITE_CH2:
    case CMD_READ_CH2:
        return 3;
    case CMD_MBENABLE:
        return 2;
    default:
        return 0;
    }
}

/* Run a command once all its parameter bytes are in. */
static void aha_run_cmd(AHA154xState *s)
{
    uint8_t *c = s->cmdbuf;
    uint8_t *d = s->databuf;
    bool suppress = false;
    int i;

    trace_aha154x_cmd(s->cmd, c[0], c[1], c[2], c[3]);
    s->datapos = 0;
    s->dataleft = 0;

    switch (s->cmd) {
    case CMD_NOP:
    case CMD_SELTIMEOUT:
        break;
    case CMD_MBINIT:
        s->mb_count = c[0];
        s->mbo_addr = get24(&c[1]);
        s->mbi_addr = s->mbo_addr + s->mb_count * 4;
        s->mbo_pos = 0;
        s->mbi_pos = 0;
        s->status &= ~STAT_INIT;
        trace_aha154x_mbinit(s->mb_count, s->mbo_addr);
        break;
    case CMD_BIOSCMD:
        aha_bios_cmd(s);
        if (s->bios_busy) {
            return;
        }
        break;
    case CMD_INQUIRY:
        memcpy(d, s->fw, 4);
        s->dataleft = 4;
        break;
    case CMD_EMBOI:
        if (c[0] <= 1) {
            s->mbo_intr = c[0];
            suppress = true;
        } else {
            s->status |= STAT_INVCMD;
        }
        break;
    case CMD_BUSON_TIME:
        s->bus_on = c[0];
        break;
    case CMD_BUSOFF_TIME:
        s->bus_off = c[0];
        break;
    case CMD_DMASPEED:
        s->dma_speed = c[0];
        break;
    case CMD_RETDEVS:
        for (i = 0; i < 8; i++) {
            d[i] = (i != s->host_id && aha_find(s, i, 0)) ? 1 : 0;
        }
        s->dataleft = 8;
        break;
    case CMD_RETCONF:
        d[0] = 1 << s->isadma;
        d[1] = s->isairq >= 9 ? 1 << (s->isairq - 9) : 0;
        d[2] = s->host_id;
        s->dataleft = 3;
        break;
    case CMD_RETSETUP:
        memset(d, 0, DATABUF_LEN);
        d[1] = s->dma_speed;
        d[2] = s->bus_on;
        d[3] = s->bus_off;
        d[4] = s->mb_count;
        put24(&d[5], s->mbo_addr);
        /* vendor data: signature, retry, switches, checksum, BIOS mbox */
        d[17 + 22] = 0xa3;
        d[17 + 23] = 0xc2;
        s->dataleft = c[0];
        break;
    case CMD_ECHO:
        d[0] = c[0];
        s->dataleft = 1;
        break;
    case CMD_WRITE_CH2:
        aha_read(get24(c), s->ch2buf, 64);
        break;
    case CMD_READ_CH2:
        aha_write(get24(c), s->ch2buf, 64);
        break;
    case CMD_OPTIONS:
        if (s->cmdparam == 1 && c[0]) {
            /* c[0] more bytes follow */
            s->cmdleft = MIN(c[0], (int)sizeof(s->cmdbuf) - 1);
            return;
        }
        break;
    case CMD_EXTBIOS:
        /*
         * BIOS 3.20 asks this at every boot and waits seconds for an
         * answer. Bit 3 set would mean 255/63 translation, and the BIOS
         * would warn about the 64/32 disks; the mailbox is never locked.
         */
        d[0] = 0x00;
        d[1] = 0x00;
        s->dataleft = 2;
        break;
    case CMD_MBENABLE:
        break;
    default:
        /* the 1542C's EEPROM and shadow RAM commands */
        qemu_log_mask(LOG_UNIMP, "aha154x: unknown command 0x%02x\n", s->cmd);
        s->status |= STAT_INVCMD;
        break;
    }

    if (s->dataleft) {
        s->status |= STAT_DF;
    } else {
        aha_cmd_done(s, suppress);
    }
}

static void aha_write_data(AHA154xState *s, uint8_t val)
{
    if (s->cmd == 0xff) {
        if (val == CMD_START_SCSI) {
            qemu_bh_schedule(s->mbo_bh);
            return;
        }
        s->cmd = val;
        s->cmdparam = 0;
        s->cmdleft = aha_param_len(val);
        s->status &= ~(STAT_INVCMD | STAT_IDLE);
    } else if (s->cmdleft > 0) {
        if (s->cmdparam < (int)sizeof(s->cmdbuf)) {
            s->cmdbuf[s->cmdparam++] = val;
        }
        s->cmdleft--;
    } else {
        /* A reply is not read yet, or the BIOS command still runs. */
        qemu_log_mask(LOG_GUEST_ERROR,
                      "aha154x: byte 0x%02x while command 0x%02x runs\n",
                      val, s->cmd);
        return;
    }
    if (!s->cmdleft) {
        aha_run_cmd(s);
    }
}

static uint64_t aha_ioport_read(void *opaque, hwaddr addr, unsigned size)
{
    AHA154xState *s = opaque;
    uint8_t ret;

    switch (addr & 3) {
    case 0:
        ret = s->status;
        break;
    case 1:
        ret = s->databuf[s->datapos % DATABUF_LEN];
        if (s->dataleft) {
            s->datapos++;
            if (!--s->dataleft) {
                aha_cmd_done(s, false);
            }
        }
        break;
    case 2:
        ret = s->intr & ~0x70;
        break;
    default:
        ret = 0xff;
        break;
    }
    trace_aha154x_ioport_read(addr & 3, ret);
    return ret;
}

static void aha_ioport_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    AHA154xState *s = opaque;

    trace_aha154x_ioport_write(addr & 3, val);
    switch (addr & 3) {
    case 0:
        if (val & (CTRL_HRST | CTRL_SRST)) {
            aha_reset_ctrl(s, val & CTRL_HRST);
            break;
        }
        if (val & CTRL_SCRST) {
            aha_cancel_all(s);
            bus_cold_reset(BUS(&s->bus));
        }
        if (val & CTRL_IRST) {
            aha_clear_irq(s);
        }
        break;
    case 1:
        aha_write_data(s, val);
        break;
    default:
        break;
    }
}

static const MemoryRegionOps aha_ioport_ops = {
    .read = aha_ioport_read,
    .write = aha_ioport_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 1,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static const struct SCSIBusInfo aha_scsi_info = {
    .tcq = false,
    .max_target = 7,
    .max_lun = 7,
    .transfer_data = aha_transfer_data,
    .complete = aha_complete,
    .cancel = aha_cancel,
    .get_sg_list = aha_get_sg_list,
};

static void aha_realize(DeviceState *dev, Error **errp)
{
    ISADevice *isadev = ISA_DEVICE(dev);
    AHA154xState *s = AHA154X(dev);

    if (s->fwrev && strlen(s->fwrev) != 4) {
        error_setg(errp, "fwrev must be 4 characters, such as A014");
        return;
    }
    memcpy(s->fw, s->fwrev ? s->fwrev : "A014", 4);
    if (s->isadma > 7 || s->host_id > 7) {
        error_setg(errp, "dma and scsi_id must be 0 to 7");
        return;
    }
    memory_region_init_io(&s->io, OBJECT(s), &aha_ioport_ops, s,
                          "aha154x", 4);
    isa_register_ioport(isadev, &s->io, s->iobase);
    s->irq = isa_get_irq(isadev, s->isairq);
    s->mbo_bh = qemu_bh_new_guarded(aha_mbo_scan, s,
                                    &dev->mem_reentrancy_guard);
    s->reset_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, aha_reset_done, s);
    QTAILQ_INIT(&s->reqs);
    scsi_bus_init(&s->bus, sizeof(s->bus), dev, &aha_scsi_info);
    if (s->romfile && rom_add_option(s->romfile, -1) < 0) {
        error_setg(errp, "could not load the BIOS '%s'", s->romfile);
        return;
    }
}

static void aha_reset(DeviceState *dev)
{
    AHA154xState *s = AHA154X(dev);

    aha_reset_state(s);
    timer_del(s->reset_timer);
    s->status = STAT_INIT | STAT_IDLE;
}

static const Property aha_properties[] = {
    DEFINE_PROP_UINT32("iobase", AHA154xState, iobase, 0x330),
    DEFINE_PROP_UINT32("irq", AHA154xState, isairq, 11),
    DEFINE_PROP_UINT32("dma", AHA154xState, isadma, 5),
    DEFINE_PROP_UINT8("scsi_id", AHA154xState, host_id, 7),
    DEFINE_PROP_STRING("romfile", AHA154xState, romfile),
    DEFINE_PROP_STRING("fwrev", AHA154xState, fwrev),
};

static const VMStateDescription vmstate_aha154x = {
    .name = "aha154x",
    .unmigratable = 1,
};

static void aha_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = aha_realize;
    device_class_set_legacy_reset(dc, aha_reset);
    device_class_set_props(dc, aha_properties);
    dc->vmsd = &vmstate_aha154x;
    dc->desc = "Adaptec AHA-154xB ISA SCSI host adapter";
    set_bit(DEVICE_CATEGORY_STORAGE, dc->categories);
}

static const TypeInfo aha_info = {
    .name = TYPE_AHA154X,
    .parent = TYPE_ISA_DEVICE,
    .instance_size = sizeof(AHA154xState),
    .class_init = aha_class_init,
};

static void aha_register_types(void)
{
    type_register_static(&aha_info);
}

type_init(aha_register_types)
