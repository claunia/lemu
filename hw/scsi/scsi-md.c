/*
 * Sony MD DATA (MiniDisc) SCSI drives: the MDM-110 mechanism and the
 * MDH-10 external drive.
 *
 * Reconstructed from bus captures of a real MDH-10, raw responses of a
 * real MDM-110, and Sony's MD DATA drivers for DOS, Windows 95 and Mac OS.
 * The same model is in 86Box (src/disk/md.c) and Cockatrice III
 * (BasiliskII/scsi/scsi_mdh10.cpp).
 *
 * The disc is an image of its 70464 logical blocks of 2048 bytes. The
 * User TOC, which is outside the logical blocks, is kept next to it as
 * <image without extension>.utoc.bin, as Aaru reads and writes it.
 *
 * Copyright (c) 2026 Natalia Portillo
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/memalign.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qemu/cutils.h"
#include "qemu/bswap.h"
#include "hw/scsi/scsi.h"
#include "migration/vmstate.h"
#include "scsi/constants.h"
#include "system/block-backend.h"
#include "hw/block/block.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "trace.h"
#include "qom/object.h"

#define TYPE_SCSI_MD "scsi-md"
OBJECT_DECLARE_SIMPLE_TYPE(SCSIMDState, SCSI_MD)

/* Peripheral device type, as the drive reports it */
#define MD_DEVICE_TYPE      0x10

/* MD DATA discs hold 70464 logical blocks of 2048 bytes */
#define MD_BLOCK_SIZE       2048
#define MD_BLOCKS           70464

#define MD_BUF_SIZE         (128 * KiB)
#define MD_SEEK_6           0x0b

/* Sense keys */
#define MD_SK_NO_SENSE          0x0
#define MD_SK_NOT_READY         0x2
#define MD_SK_MEDIUM_ERROR      0x3
#define MD_SK_ILLEGAL_REQUEST   0x5
#define MD_SK_UNIT_ATTENTION    0x6
#define MD_SK_DATA_PROTECT      0x7

/* Command flags */
#define MD_IMPLEMENTED  1
#define MD_CHECK_READY  2
#define MD_ALLOW_UA     4

static const uint8_t md_command_flags[0x100] = {
    [0x00] = MD_IMPLEMENTED | MD_CHECK_READY,   /* TEST UNIT READY */
    [0x01] = MD_IMPLEMENTED | MD_CHECK_READY,   /* REZERO UNIT */
    [0x03] = MD_IMPLEMENTED | MD_ALLOW_UA,      /* REQUEST SENSE */
    [0x08] = MD_IMPLEMENTED | MD_CHECK_READY,   /* READ (6) */
    [0x0a] = MD_IMPLEMENTED | MD_CHECK_READY,   /* WRITE (6) */
    [0x0b] = MD_IMPLEMENTED | MD_CHECK_READY,   /* SEEK (6) */
    [0x12] = MD_IMPLEMENTED | MD_ALLOW_UA,      /* INQUIRY */
    [0x16] = MD_IMPLEMENTED,                    /* RESERVE */
    [0x17] = MD_IMPLEMENTED,                    /* RELEASE */
    [0x1b] = MD_IMPLEMENTED,                    /* START STOP UNIT */
    [0x1d] = MD_IMPLEMENTED,                    /* SEND DIAGNOSTIC */
    [0x1e] = MD_IMPLEMENTED,                    /* PREVENT/ALLOW MEDIUM REMOVAL */
    [0x25] = MD_IMPLEMENTED | MD_CHECK_READY,   /* READ CAPACITY */
    [0x28] = MD_IMPLEMENTED | MD_CHECK_READY,   /* READ (10) */
    [0x2a] = MD_IMPLEMENTED | MD_CHECK_READY,   /* WRITE (10) */
    [0x2b] = MD_IMPLEMENTED | MD_CHECK_READY,   /* SEEK (10) */
    [0x2e] = MD_IMPLEMENTED | MD_CHECK_READY,   /* WRITE AND VERIFY (10) */
    [0x2f] = MD_IMPLEMENTED | MD_CHECK_READY,   /* VERIFY (10) */
    [0x34] = MD_IMPLEMENTED | MD_CHECK_READY,   /* PRE-FETCH */
    [0x35] = MD_IMPLEMENTED | MD_CHECK_READY,   /* SYNCHRONIZE CACHE */
    [0x36] = MD_IMPLEMENTED | MD_CHECK_READY,   /* LOCK/UNLOCK CACHE */
    [0x45] = MD_IMPLEMENTED | MD_CHECK_READY,   /* PLAY AUDIO (10) */
    [0x48] = MD_IMPLEMENTED | MD_CHECK_READY,   /* PLAY AUDIO TRACK/INDEX */
    [0x4b] = MD_IMPLEMENTED | MD_CHECK_READY,   /* PAUSE/RESUME */
};

typedef struct MDModel {
    const char *name;
    const char *product;
    const char *revision;
} MDModel;

static const MDModel md_models[] = {
    { .name = "mdm110", .product = "MDM110", .revision = "1.01" },
    { .name = "mdh-10", .product = "MDH-10", .revision = "1.11" },
};

struct SCSIMDState {
    SCSIDevice qdev;

    char *model_name;
    const MDModel *model;

    bool tray_open;
    bool tray_locked;
    /* A disc went in: the next command gets a UNIT ATTENTION */
    bool media_changed;
};

typedef struct SCSIMDReq {
    SCSIRequest req;
    uint8_t *buf;
    uint32_t buflen;
    /* Emulated commands: bytes still to hand to the HBA, or to get from it */
    uint32_t len;
    /* READ and WRITE: next block and blocks left */
    uint32_t lba;
    uint32_t count;
    bool started;
    struct iovec iov;
    QEMUIOVector qiov;
    BlockAcctCookie acct;
} SCSIMDReq;

/* Disc */

static bool md_present(SCSIMDState *s)
{
    return blk_is_inserted(s->qdev.conf.blk) && !s->tray_open;
}

static bool md_read_only(SCSIMDState *s)
{
    return !blk_is_writable(s->qdev.conf.blk);
}

static uint32_t md_blocks(SCSIMDState *s)
{
    int64_t len = blk_getlength(s->qdev.conf.blk);

    if (len < 0) {
        return 0;
    }
    return MIN(len / MD_BLOCK_SIZE, MD_BLOCKS);
}

/* Sense */

/* Finishes the command with fixed-format sense as the drive returns it */
static int32_t md_check(SCSIMDReq *r, uint8_t key, uint8_t asc, uint8_t ascq,
                        uint8_t fru, bool info_valid, uint32_t info,
                        const uint8_t *sks)
{
    uint8_t *sense = r->req.sense;

    memset(sense, 0, 18);
    sense[0] = info_valid ? 0xf0 : 0x70;
    sense[2] = key;
    if (info_valid) {
        stl_be_p(sense + 3, info);
    }
    sense[7] = 10;
    sense[12] = asc;
    sense[13] = ascq;
    sense[14] = fru;
    if (sks) {
        memcpy(sense + 15, sks, 3);
    }
    r->req.sense_len = 18;

    trace_scsi_md_check(r->req.cmd.buf[0], key, asc, ascq);
    scsi_req_complete(&r->req, CHECK_CONDITION);
    return 0;
}

static int32_t md_invalid_field(SCSIMDReq *r, int byte)
{
    const uint8_t sks[3] = { 0xc0, 0x00, byte };

    return md_check(r, MD_SK_ILLEGAL_REQUEST, 0x24, 0, 0xc7, false, 0, sks);
}

static int32_t md_lba_out_of_range(SCSIMDReq *r, uint32_t lba)
{
    return md_check(r, MD_SK_ILLEGAL_REQUEST, 0x21, 0, 0, true, lba, NULL);
}

static int32_t md_write_protected(SCSIMDReq *r)
{
    return md_check(r, MD_SK_DATA_PROTECT, 0x27, 0, 0, false, 0, NULL);
}

/* Request buffers */

static uint8_t *md_alloc_buf(SCSIMDReq *r, uint32_t len)
{
    if (r->buflen < len) {
        qemu_vfree(r->buf);
        r->buf = blk_blockalign(r->req.dev->conf.blk, len);
        r->buflen = len;
    }
    memset(r->buf, 0, len);
    return r->buf;
}

/* Hands len bytes of r->buf to the host, at most the allocation length */
static int32_t md_data_in(SCSIMDReq *r, uint32_t len)
{
    len = MIN(len, r->req.cmd.xfer);
    if (len == 0) {
        scsi_req_complete(&r->req, GOOD);
        return 0;
    }
    r->len = len;
    return len;
}

/* Gets the command's data from the host, then runs md_emulate_write_done() */
static int32_t md_data_out(SCSIMDReq *r)
{
    uint32_t len = r->req.cmd.xfer;

    if (len == 0) {
        scsi_req_complete(&r->req, GOOD);
        return 0;
    }
    md_alloc_buf(r, len);
    r->len = len;
    return -(int32_t)len;
}

/* Commands */

static void md_trace_cdb(SCSIMDReq *r)
{
    if (trace_event_get_state_backends(TRACE_SCSI_MD_COMMAND)) {
        g_autoptr(GString) str = qemu_hexdump_line(NULL, r->req.cmd.buf,
                                                   r->req.cmd.len, 1, 0);

        trace_scsi_md_command(r->req.dev->id, str->str);
    }
}

static int32_t md_send_command(SCSIRequest *req, uint8_t *cdb)
{
    SCSIMDReq *r = DO_UPCAST(SCSIMDReq, req, req);
    SCSIMDState *s = SCSI_MD(req->dev);
    uint8_t op = cdb[0];
    uint8_t flags = md_command_flags[op];
    uint32_t blocks = 0;
    uint8_t *buf;
    int32_t start;

    md_trace_cdb(r);

    if (!(flags & MD_IMPLEMENTED)) {
        return md_check(r, MD_SK_ILLEGAL_REQUEST, 0x20, 0, 0, false, 0, NULL);
    }

    if (!(flags & MD_ALLOW_UA)) {
        if (s->media_changed && md_present(s)) {
            s->media_changed = false;
            /* Not ready to ready change, medium may have changed */
            return md_check(r, MD_SK_UNIT_ATTENTION, 0x28, 0, 0xcc, false, 0,
                            NULL);
        }
    }

    if (flags & MD_CHECK_READY) {
        if (!md_present(s)) {
            return md_check(r, MD_SK_NOT_READY, 0x3a, 0, 0, false, 0, NULL);
        }
        blocks = md_blocks(s);
    }

    switch (op) {
    case TEST_UNIT_READY:
    case REZERO_UNIT:
    case RESERVE:
    case RELEASE:
    case PRE_FETCH:
    case LOCK_UNLOCK_CACHE:
    case 0x45:  /* PLAY AUDIO (10): audio is not emulated */
    case 0x48:  /* PLAY AUDIO TRACK/INDEX */
    case 0x4b:  /* PAUSE/RESUME */
        scsi_req_complete(&r->req, GOOD);
        return 0;

    case SEND_DIAGNOSTIC:
        if (r->req.cmd.xfer) {
            return md_data_out(r);
        }
        scsi_req_complete(&r->req, GOOD);
        return 0;

    case SYNCHRONIZE_CACHE:
        blk_flush(s->qdev.conf.blk);
        scsi_req_complete(&r->req, GOOD);
        return 0;

    case REQUEST_SENSE:
        buf = md_alloc_buf(r, 18);
        if (s->media_changed && md_present(s)) {
            s->media_changed = false;
            buf[0] = 0x70;
            buf[2] = MD_SK_UNIT_ATTENTION;
            buf[7] = 10;
            buf[12] = 0x28;
            buf[14] = 0xcc;
        } else {
            buf[0] = 0x70;
            buf[7] = 10;
        }
        return md_data_in(r, 18);

    case INQUIRY:
        if (cdb[1] & 1) {
            return md_invalid_field(r, 1);
        }
        buf = md_alloc_buf(r, 36);
        buf[0] = MD_DEVICE_TYPE;
        buf[1] = 0x85;      /* Removable, type modifier 5 */
        buf[2] = 0x02;      /* SCSI-2 */
        buf[3] = 0x01;      /* Response data format */
        buf[4] = 31;
        strpadcpy((char *)buf + 8, 8, "SONY", ' ');
        strpadcpy((char *)buf + 16, 16, s->model->product, ' ');
        strpadcpy((char *)buf + 32, 4, s->model->revision, ' ');
        return md_data_in(r, 36);

    case READ_6:
    case READ_10:
        if (op == READ_6) {
            r->lba = ((cdb[1] & 0x1f) << 16) | (cdb[2] << 8) | cdb[3];
            r->count = cdb[4] ? cdb[4] : 256;
        } else {
            r->lba = ldl_be_p(cdb + 2);
            r->count = lduw_be_p(cdb + 7);
        }
        if ((uint64_t)r->lba + r->count > blocks) {
            return md_lba_out_of_range(r, r->lba);
        }
        if (r->count == 0) {
            scsi_req_complete(&r->req, GOOD);
            return 0;
        }
        return r->count * MD_BLOCK_SIZE;

    case WRITE_6:
    case WRITE_10:
    case WRITE_VERIFY_10:
        if (md_read_only(s)) {
            return md_write_protected(r);
        }
        if (op == WRITE_6) {
            r->lba = ((cdb[1] & 0x1f) << 16) | (cdb[2] << 8) | cdb[3];
            r->count = cdb[4] ? cdb[4] : 256;
        } else {
            /* Control byte bit 7 is a hint for new clusters: it changes nothing */
            r->lba = ldl_be_p(cdb + 2);
            r->count = lduw_be_p(cdb + 7);
        }
        if ((uint64_t)r->lba + r->count > blocks) {
            return md_lba_out_of_range(r, r->lba);
        }
        if (r->count == 0) {
            scsi_req_complete(&r->req, GOOD);
            return 0;
        }
        return -(int32_t)(r->count * MD_BLOCK_SIZE);

    case VERIFY_10:
        if ((uint64_t)ldl_be_p(cdb + 2) + lduw_be_p(cdb + 7) > blocks) {
            return md_lba_out_of_range(r, ldl_be_p(cdb + 2));
        }
        if (r->req.cmd.xfer) {
            /* Byte compare: take the data and accept it */
            return md_data_out(r);
        }
        scsi_req_complete(&r->req, GOOD);
        return 0;

    case MD_SEEK_6:
    case SEEK_10:
        start = (op == MD_SEEK_6) ? ((cdb[1] & 0x1f) << 16) | (cdb[2] << 8) | cdb[3]
                               : ldl_be_p(cdb + 2);
        if ((uint32_t)start >= blocks) {
            return md_lba_out_of_range(r, start);
        }
        scsi_req_complete(&r->req, GOOD);
        return 0;

    case READ_CAPACITY_10:
        buf = md_alloc_buf(r, 8);
        stl_be_p(buf, blocks - 1);       /* The last block */
        stl_be_p(buf + 4, MD_BLOCK_SIZE);
        return md_data_in(r, 8);

    case START_STOP:
        if ((cdb[4] & 3) == 2) {
            /* Eject */
            if (s->tray_locked) {
                /* Medium removal prevented */
                return md_check(r, MD_SK_ILLEGAL_REQUEST, 0x53, 0x02, 0, false,
                                0, NULL);
            }
            if (!s->tray_open) {
                blk_eject(s->qdev.conf.blk, true);
                s->tray_open = true;
                trace_scsi_md_tray(true);
            }
        } else if ((cdb[4] & 3) == 3 && s->tray_open) {
            /* Load: the disc goes back in */
            blk_eject(s->qdev.conf.blk, false);
            s->tray_open = false;
            s->media_changed = true;
                    trace_scsi_md_tray(false);
        }
        scsi_req_complete(&r->req, GOOD);
        return 0;

    case ALLOW_MEDIUM_REMOVAL:
        s->tray_locked = cdb[4] & 1;
        blk_lock_medium(s->qdev.conf.blk, s->tray_locked);
        scsi_req_complete(&r->req, GOOD);
        return 0;

    default:
        return md_check(r, MD_SK_ILLEGAL_REQUEST, 0x20, 0, 0, false, 0, NULL);
    }
}

/* Data from the host for an emulated command has arrived */
static void md_emulate_write_done(SCSIMDReq *r, SCSIMDState *s)
{
    /* SEND DIAGNOSTIC and VERIFY with byte compare: accepted */
    scsi_req_complete(&r->req, GOOD);
}

/* Disc I/O */

static bool md_is_rw(SCSIMDReq *r)
{
    switch (r->req.cmd.buf[0]) {
    case READ_6:
    case READ_10:
    case WRITE_6:
    case WRITE_10:
    case WRITE_VERIFY_10:
        return true;
    default:
        return false;
    }
}

static void md_init_chunk(SCSIMDReq *r)
{
    uint32_t len = MIN((uint64_t)r->count * MD_BLOCK_SIZE, MD_BUF_SIZE);

    if (r->buflen < MD_BUF_SIZE) {
        qemu_vfree(r->buf);
        r->buf = blk_blockalign(r->req.dev->conf.blk, MD_BUF_SIZE);
        r->buflen = MD_BUF_SIZE;
    }
    r->iov.iov_base = r->buf;
    r->iov.iov_len = len;
    qemu_iovec_init_external(&r->qiov, &r->iov, 1);
}

/* Returns true when the request is finished with */
static bool md_rw_failed(SCSIMDReq *r, int ret)
{
    SCSIMDState *s = SCSI_MD(r->req.dev);
    bool is_read = r->req.cmd.mode == SCSI_XFER_FROM_DEV;

    if (r->req.io_canceled) {
        scsi_req_cancel_complete(&r->req);
        return true;
    }
    if (ret == 0) {
        block_acct_done(blk_get_stats(s->qdev.conf.blk), &r->acct);
        return false;
    }

    block_acct_failed(blk_get_stats(s->qdev.conf.blk), &r->acct);
    if (ret == -ENOMEDIUM) {
        md_check(r, MD_SK_NOT_READY, 0x3a, 0, 0, false, 0, NULL);
    } else if (is_read) {
        md_check(r, MD_SK_MEDIUM_ERROR, 0x11, 0, 0, true, r->lba, NULL);
    } else {
        md_check(r, MD_SK_MEDIUM_ERROR, 0x0c, 0, 0, true, r->lba, NULL);
    }
    return true;
}

static void md_read_complete(void *opaque, int ret)
{
    SCSIMDReq *r = opaque;
    uint32_t n;

    assert(r->req.aiocb != NULL);
    r->req.aiocb = NULL;

    if (!md_rw_failed(r, ret)) {
        n = r->qiov.size / MD_BLOCK_SIZE;
        r->lba += n;
        r->count -= n;
        scsi_req_data(&r->req, r->qiov.size);
    }
    scsi_req_unref(&r->req);
}

static void md_write_complete(void *opaque, int ret)
{
    SCSIMDReq *r = opaque;
    uint32_t n;

    assert(r->req.aiocb != NULL);
    r->req.aiocb = NULL;

    if (!md_rw_failed(r, ret)) {
        n = r->qiov.size / MD_BLOCK_SIZE;
        r->lba += n;
        r->count -= n;
        if (r->count == 0) {
            scsi_req_complete(&r->req, GOOD);
        } else {
            md_init_chunk(r);
            scsi_req_data(&r->req, r->qiov.size);
        }
    }
    scsi_req_unref(&r->req);
}

static void md_read_data(SCSIRequest *req)
{
    SCSIMDReq *r = DO_UPCAST(SCSIMDReq, req, req);
    SCSIMDState *s = SCSI_MD(req->dev);

    if (!md_is_rw(r)) {
        if (r->len) {
            uint32_t len = r->len;

            r->len = 0;
            scsi_req_data(&r->req, len);
            return;
        }
        scsi_req_complete(&r->req, GOOD);
        return;
    }

    if (r->count == 0) {
        scsi_req_complete(&r->req, GOOD);
        return;
    }
    if (!md_present(s)) {
        md_check(r, MD_SK_NOT_READY, 0x3a, 0, 0, false, 0, NULL);
        return;
    }

    md_init_chunk(r);
    scsi_req_ref(&r->req);
    block_acct_start(blk_get_stats(s->qdev.conf.blk), &r->acct,
                     r->qiov.size, BLOCK_ACCT_READ);
    r->req.aiocb = blk_aio_preadv(s->qdev.conf.blk,
                                  (int64_t)r->lba * MD_BLOCK_SIZE, &r->qiov,
                                  0, md_read_complete, r);
}

static void md_write_data(SCSIRequest *req)
{
    SCSIMDReq *r = DO_UPCAST(SCSIMDReq, req, req);
    SCSIMDState *s = SCSI_MD(req->dev);

    if (!md_is_rw(r)) {
        if (!r->started) {
            /* Ask the host for the data */
            r->started = true;
            scsi_req_data(&r->req, r->len);
            return;
        }
        md_emulate_write_done(r, s);
        return;
    }

    if (!r->started) {
        r->started = true;
        md_init_chunk(r);
        scsi_req_data(&r->req, r->qiov.size);
        return;
    }
    if (!md_present(s)) {
        md_check(r, MD_SK_NOT_READY, 0x3a, 0, 0, false, 0, NULL);
        return;
    }

    scsi_req_ref(&r->req);
    block_acct_start(blk_get_stats(s->qdev.conf.blk), &r->acct,
                     r->qiov.size, BLOCK_ACCT_WRITE);
    r->req.aiocb = blk_aio_pwritev(s->qdev.conf.blk,
                                   (int64_t)r->lba * MD_BLOCK_SIZE, &r->qiov,
                                   0, md_write_complete, r);
}

static uint8_t *md_get_buf(SCSIRequest *req)
{
    SCSIMDReq *r = DO_UPCAST(SCSIMDReq, req, req);

    return r->buf;
}

static void md_free_request(SCSIRequest *req)
{
    SCSIMDReq *r = DO_UPCAST(SCSIMDReq, req, req);

    qemu_vfree(r->buf);
}

static const SCSIReqOps md_reqops = {
    .size         = sizeof(SCSIMDReq),
    .free_req     = md_free_request,
    .send_command = md_send_command,
    .read_data    = md_read_data,
    .write_data   = md_write_data,
    .get_buf      = md_get_buf,
};

static SCSIRequest *md_new_request(SCSIDevice *d, uint32_t tag, uint32_t lun,
                                   uint8_t *buf, void *hba_private)
{
    return scsi_req_alloc(&md_reqops, d, tag, lun, hba_private);
}

/* The audio commands move no data, whatever their length fields say */
static int md_parse_cdb(SCSIDevice *dev, SCSICommand *cmd, uint8_t *buf,
                        size_t buf_len, void *hba_private)
{
    int ret;

    ret = scsi_req_parse_cdb(dev, cmd, buf, buf_len);
    if (ret) {
        return ret;
    }

    switch (buf[0]) {
    case REZERO_UNIT:
    case MD_SEEK_6:
    case 0x45:  /* PLAY AUDIO (10): the length is in blocks, nothing moves */
    case 0x48:  /* PLAY AUDIO TRACK/INDEX */
    case 0x4b:  /* PAUSE/RESUME: byte 8 is the resume bit */
        cmd->xfer = 0;
        cmd->mode = SCSI_XFER_NONE;
        break;
    default:
        break;
    }
    return 0;
}

/* Removable medium */

static void md_change_media_cb(void *opaque, bool load, Error **errp)
{
    SCSIMDState *s = opaque;

    s->media_changed = load;
    s->tray_open = !load;
    trace_scsi_md_media(load);
}

static void md_eject_request_cb(void *opaque, bool force)
{
    SCSIMDState *s = opaque;

    if (force) {
        s->tray_locked = false;
    }
}

static bool md_is_tray_open(void *opaque)
{
    return ((SCSIMDState *)opaque)->tray_open;
}

static bool md_is_medium_locked(void *opaque)
{
    return ((SCSIMDState *)opaque)->tray_locked;
}

static void md_drained_begin(void *opaque)
{
    SCSIMDState *s = opaque;

    scsi_device_drained_begin(&s->qdev);
}

static void md_drained_end(void *opaque)
{
    SCSIMDState *s = opaque;

    scsi_device_drained_end(&s->qdev);
}

static const BlockDevOps md_block_ops = {
    .change_media_cb  = md_change_media_cb,
    .eject_request_cb = md_eject_request_cb,
    .is_tray_open     = md_is_tray_open,
    .is_medium_locked = md_is_medium_locked,
    .drained_begin    = md_drained_begin,
    .drained_end      = md_drained_end,
};

/* Device */

static void md_reset(DeviceState *dev)
{
    SCSIMDState *s = SCSI_MD(dev);

    scsi_device_purge_requests(&s->qdev, SENSE_CODE(RESET));

    /* A disc taken out by the guest is back in after a reset */
    if (s->tray_open) {
        s->tray_open = false;
        s->media_changed = true;
        }
    s->tray_locked = false;
}

static void md_realize(SCSIDevice *dev, Error **errp)
{
    SCSIMDState *s = SCSI_MD(dev);
    int ret;

    s->model = NULL;
    for (int i = 0; i < ARRAY_SIZE(md_models); i++) {
        if (!g_ascii_strcasecmp(s->model_name, md_models[i].name) ||
            !g_ascii_strcasecmp(s->model_name, md_models[i].product)) {
            s->model = &md_models[i];
        }
    }
    if (!s->model) {
        error_setg(errp, "unknown model '%s': use mdh-10 or mdm110",
                   s->model_name);
        return;
    }

    if (!dev->conf.blk) {
        /* An empty drive */
        dev->conf.blk = blk_new(qemu_get_aio_context(), 0, BLK_PERM_ALL);
        ret = blk_attach_dev(dev->conf.blk, &dev->qdev);
        assert(ret == 0);
    }

    if (!blkconf_apply_backend_options(&dev->conf,
                                       !blk_supports_write_perm(dev->conf.blk),
                                       false, errp)) {
        return;
    }

    if (blk_is_sg(dev->conf.blk)) {
        error_setg(errp, "unwanted /dev/sg*");
        return;
    }

    dev->type = MD_DEVICE_TYPE;
    dev->blocksize = MD_BLOCK_SIZE;
    dev->max_lba = MD_BLOCKS - 1;

    s->media_changed = blk_is_inserted(dev->conf.blk);

    blk_set_dev_ops(dev->conf.blk, &md_block_ops, s);
    blk_iostatus_enable(dev->conf.blk);
}

static const VMStateDescription vmstate_scsi_md = {
    .name = "scsi-md",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_SCSI_DEVICE(qdev, SCSIMDState),
        VMSTATE_BOOL(tray_open, SCSIMDState),
        VMSTATE_BOOL(tray_locked, SCSIMDState),
        VMSTATE_BOOL(media_changed, SCSIMDState),
        VMSTATE_END_OF_LIST()
    }
};

static const Property scsi_md_properties[] = {
    DEFINE_PROP_DRIVE("drive", SCSIMDState, qdev.conf.blk),
    DEFINE_PROP_STRING("model", SCSIMDState, model_name),
    DEFINE_BLOCK_ERROR_PROPERTIES(SCSIMDState, qdev.conf),
};

static void scsi_md_instance_init(Object *obj)
{
    SCSIMDState *s = SCSI_MD(obj);

    s->model_name = g_strdup("mdh-10");
}

static void scsi_md_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SCSIDeviceClass *sc = SCSI_DEVICE_CLASS(klass);

    sc->realize   = md_realize;
    sc->alloc_req = md_new_request;
    sc->parse_cdb = md_parse_cdb;
    dc->desc = "Sony MD DATA drive (MDH-10, MDM-110)";
    device_class_set_legacy_reset(dc, md_reset);
    device_class_set_props(dc, scsi_md_properties);
    dc->vmsd = &vmstate_scsi_md;
    set_bit(DEVICE_CATEGORY_STORAGE, dc->categories);
}

static const TypeInfo scsi_md_info = {
    .name          = TYPE_SCSI_MD,
    .parent        = TYPE_SCSI_DEVICE,
    .instance_size = sizeof(SCSIMDState),
    .instance_init = scsi_md_instance_init,
    .class_init    = scsi_md_class_init,
};

static void scsi_md_register_types(void)
{
    type_register_static(&scsi_md_info);
}

type_init(scsi_md_register_types)
