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
#include "block/block_int-common.h"
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

/* TOC sectors are raw 2352-byte sectors without their sync and header */
#define MD_TOC_SECTOR_SIZE  2336
#define MD_UTOC_SECTORS     8
#define MD_UTOC_SIZE        (MD_UTOC_SECTORS * MD_TOC_SECTOR_SIZE)

/* Volume descriptor of the MiniDisc File System, 32 blocks into the data track */
#define MD_VD_LBA           0x420
/* Clusters of the system and data tracks */
#define MD_FIRST_CLUSTER    50
#define MD_DATA_CLUSTER     82
#define MD_LEADOUT          2252

#define MD_BUF_SIZE         (128 * KiB)
#define MD_PAGE_LEN         16
#define MD_PAGES            0x40
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
    [0x15] = MD_IMPLEMENTED,                    /* MODE SELECT (6) */
    [0x16] = MD_IMPLEMENTED,                    /* RESERVE */
    [0x17] = MD_IMPLEMENTED,                    /* RELEASE */
    [0x1a] = MD_IMPLEMENTED,                    /* MODE SENSE (6) */
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
    [0xd1] = MD_IMPLEMENTED | MD_CHECK_READY,   /* Read U-TOC */
    [0xd2] = MD_IMPLEMENTED | MD_CHECK_READY,   /* Write U-TOC */
    [0xd4] = MD_IMPLEMENTED | MD_CHECK_READY,   /* Read P-TOC */
    [0xd5] = MD_IMPLEMENTED | MD_CHECK_READY,   /* Format result */
    [0xd6] = MD_IMPLEMENTED,                    /* Stop audio playback */
    [0xd7] = MD_IMPLEMENTED,                    /* Audio play position */
    [0xd8] = MD_IMPLEMENTED | MD_CHECK_READY,   /* Disc type */
};

typedef struct MDModel {
    const char *name;
    const char *product;
    const char *revision;
    /* Mode pages: page code, length, parameters */
    uint8_t pages[MD_PAGES][MD_PAGE_LEN];
} MDModel;

/*
 * The mode pages of the MDM-110 as it returns them (Aaru device report),
 * and of the MDH-10 as captured on the bus from a real drive.
 */
static const MDModel md_models[] = {
    {
        .name = "mdm110", .product = "MDM110", .revision = "1.01",
        .pages = {
            [0x01] = { 0x01, 0x0a, 0x80, 0x03, 0x00, 0x00, 0x00, 0x00,
                       0x03, 0x00, 0x00, 0x00 },
            [0x07] = { 0x07, 0x0a, 0x00, 0x03 },
            [0x08] = { 0x08, 0x0a },
            [0x20] = { 0x20, 0x0e, 0x00, 0x00, 0x00, 0x20, 0x08, 0x00,
                       0x64, 0x14 },
        },
    },
    {
        .name = "mdh-10", .product = "MDH-10", .revision = "1.11",
        .pages = {
            [0x01] = { 0x01, 0x0a, 0x80, 0x05, 0x00, 0x00, 0x00, 0x00,
                       0x05, 0x00, 0x00, 0x00 },
            [0x07] = { 0x07, 0x0a, 0x00, 0x03 },
            [0x08] = { 0x08, 0x0a, 0x04 },
            [0x20] = { 0x20, 0x0e, 0x00, 0x00, 0x00, 0x20, 0x09, 0x20,
                       0x25 },
            /* Audio volume, channels 1 and 2 */
            [0x21] = { 0x21, 0x0e, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00,
                       0x01, 0x10, 0x02, 0x10 },
        },
    },
};

/* What MODE SELECT can change, as the MDM-110 reports it */
static const uint8_t md_pages_changeable[MD_PAGES][MD_PAGE_LEN] = {
    [0x01] = { 0x01, 0x0a, 0xb7, 0xff, 0x00, 0x00, 0x00, 0x00, 0xff },
    [0x07] = { 0x07, 0x0a, 0x07, 0xff },
    [0x08] = { 0x08, 0x0a, 0x05 },
    [0x20] = { 0x20, 0x0e, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff },
    [0x21] = { 0x21, 0x0e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff,
               0x00, 0xff },
};

struct SCSIMDState {
    SCSIDevice qdev;

    char *model_name;
    char *utoc_prop;
    const MDModel *model;

    bool tray_open;
    bool tray_locked;
    /* A disc went in: the next command gets a UNIT ATTENTION */
    bool media_changed;

    /* The U-TOC is loaded on first use after a disc goes in */
    bool utoc_valid;
    uint8_t utoc[MD_UTOC_SIZE];

    uint8_t pages[MD_PAGES][MD_PAGE_LEN];
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

/* TOC */

/* Packs a cluster, sector and sound group into a 24-bit TOC address */
static void md_toc_address(uint8_t *p, uint32_t cluster, uint32_t sector,
                           uint32_t sound_group)
{
    uint32_t addr = (cluster << 10) | (sector << 4) | sound_group;

    p[0] = addr >> 16;
    p[1] = addr >> 8;
    p[2] = addr;
}

/* P-TOC sector 0 of a recordable MD DATA disc */
static void md_build_ptoc(uint8_t *ptoc)
{
    memset(ptoc, 0, MD_TOC_SECTOR_SIZE);
    memcpy(ptoc + 0x08, "MINX", 4);     /* MD DATA */
    ptoc[0x0c] = 0x3e;                  /* Recordable disc */
    ptoc[0x0d] = 0x0d;
    ptoc[0x10] = MD_LEADOUT >> 8;       /* Lead-out cluster */
    ptoc[0x11] = MD_LEADOUT & 0xff;
    ptoc[0x13] = 0x01;                  /* Sectors in use: 0 */
    ptoc[0x15] = 0x02;
    ptoc[0x17] = 0x11;
    ptoc[0x19] = 0x03;
    ptoc[0x1d] = MD_FIRST_CLUSTER;      /* First recordable cluster */
}

/* The U-TOC written by Sony's formatters: a data track and a system track */
static void md_build_formatted_utoc(uint8_t *utoc)
{
    memset(utoc, 0, MD_UTOC_SIZE);
    utoc[0x0c] = 0x01;                  /* Maker */
    utoc[0x0d] = 0x81;                  /* Model */
    utoc[0x0e] = 1;                     /* First track */
    utoc[0x0f] = 2;                     /* Last track */
    utoc[0x13] = 0x01;                  /* Sectors in use: 0 */
    utoc[0x1f] = 3;                     /* First empty part slot */
    utoc[0x21] = 1;                     /* Track 1: part slot 1 */
    utoc[0x22] = 2;                     /* Track 2: part slot 2 */

    /* Slot 1: data track */
    md_toc_address(utoc + 0x128, MD_DATA_CLUSTER, 0, 0);
    utoc[0x12b] = 0xd0;
    md_toc_address(utoc + 0x12c, MD_LEADOUT - 1, 31, 10);

    /* Slot 2: system track */
    md_toc_address(utoc + 0x130, MD_FIRST_CLUSTER, 0, 0);
    utoc[0x133] = 0xdc;
    md_toc_address(utoc + 0x134, MD_DATA_CLUSTER - 1, 31, 10);

    /* Empty slots 3 to 255 are chained */
    for (int i = 3; i < 255; i++) {
        utoc[0x120 + 8 * i + 7] = i + 1;
    }
}

/* The U-TOC of a blank disc, and the one a formatter writes first */
static void md_build_cleared_utoc(uint8_t *utoc)
{
    memset(utoc, 0, MD_UTOC_SIZE);
    utoc[0x13] = 0x01;
}

/* The path of the U-TOC of the disc, or NULL if it has none */
static char *md_utoc_path(SCSIMDState *s)
{
    BlockDriverState *bs = blk_bs(s->qdev.conf.blk);
    const char *fn, *slash, *dot;

    if (s->utoc_prop && *s->utoc_prop) {
        return g_strdup(s->utoc_prop);
    }
    if (!bs || !bs->filename[0] || g_str_has_prefix(bs->filename, "json:")) {
        return NULL;
    }

    fn = bs->filename;
    slash = strrchr(fn, '/');
    dot = strrchr(slash ? slash : fn, '.');
    if (dot && dot > (slash ? slash + 1 : fn)) {
        return g_strdup_printf("%.*s.utoc.bin", (int)(dot - fn), fn);
    }
    return g_strdup_printf("%s.utoc.bin", fn);
}

static void md_utoc_load(SCSIMDState *s)
{
    g_autofree char *path = NULL;
    g_autofree gchar *data = NULL;
    gsize len = 0;
    uint8_t vd[8];

    if (s->utoc_valid) {
        return;
    }
    s->utoc_valid = true;

    path = md_utoc_path(s);
    if (path && g_file_get_contents(path, &data, &len, NULL) &&
        len >= MD_TOC_SECTOR_SIZE) {
        memset(s->utoc, 0, sizeof(s->utoc));
        memcpy(s->utoc, data, MIN(len, sizeof(s->utoc)));
        trace_scsi_md_utoc("loaded", path);
        return;
    }

    /* No U-TOC: a disc with an MDFS volume has the formatters' one */
    if (md_blocks(s) > MD_VD_LBA &&
        blk_pread(s->qdev.conf.blk, (int64_t)MD_VD_LBA * MD_BLOCK_SIZE,
                  sizeof(vd), vd, 0) >= 0 &&
        !memcmp(vd + 1, "MD001", 5)) {
        md_build_formatted_utoc(s->utoc);
        trace_scsi_md_utoc("made for an MDFS volume", path ? path : "");
    } else {
        md_build_cleared_utoc(s->utoc);
        trace_scsi_md_utoc("made for a blank disc", path ? path : "");
    }
}

static bool md_utoc_save(SCSIMDState *s)
{
    g_autofree char *path = md_utoc_path(s);
    g_autoptr(GError) err = NULL;
    int sectors = 1;

    if (!path) {
        /* Nowhere to keep it: it lasts until the disc goes out */
        return true;
    }

    /* Keep the sectors in use */
    for (int i = 1; i < MD_UTOC_SECTORS; i++) {
        if (s->utoc[0x13] & (1 << i)) {
            sectors = i + 1;
        }
    }

    if (!g_file_set_contents(path, (const gchar *)s->utoc,
                             sectors * MD_TOC_SECTOR_SIZE, &err)) {
        error_report("scsi-md: cannot write the U-TOC: %s", err->message);
        return false;
    }
    trace_scsi_md_utoc("saved", path);
    return true;
}

/* The first logical block of the data track from the U-TOC, or -1 */
static int32_t md_data_track_start(SCSIMDState *s)
{
    const uint8_t *utoc = s->utoc;
    int first = utoc[0x0e];
    int last = utoc[0x0f];

    if (first == 0 || last < first) {
        return -1;
    }

    for (int track = first; track <= last; track++) {
        int part = utoc[0x20 + track];
        const uint8_t *entry = utoc + 0x120 + 8 * part;
        uint32_t addr, cluster, sector;

        if (part == 0 || (entry[3] & 0x5c) != 0x50) {
            continue;
        }

        addr = (entry[0] << 16) | (entry[1] << 8) | entry[2];
        cluster = addr >> 10;
        sector = (addr >> 4) & 0x3f;
        if (cluster < MD_FIRST_CLUSTER || sector >= 32) {
            continue;
        }
        return (cluster - MD_FIRST_CLUSTER) * 32 + sector;
    }

    return -1;
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

static int32_t md_invalid_param_list(SCSIMDReq *r)
{
    return md_check(r, MD_SK_ILLEGAL_REQUEST, 0x26, 0, 0, false, 0, NULL);
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

/* Mode pages */

static uint8_t md_page_byte(SCSIMDState *s, int pc, int page, int pos)
{
    switch (pc) {
    case 1:
        return md_pages_changeable[page][pos];
    case 2:
        return s->model->pages[page][pos];
    default:
        return s->pages[page][pos];
    }
}

static bool md_page_valid(SCSIMDState *s, int page)
{
    return page < MD_PAGES && s->model->pages[page][1] != 0;
}

static int32_t md_mode_sense(SCSIMDReq *r, SCSIMDState *s, const uint8_t *cdb)
{
    bool block_desc = !(cdb[1] & 0x08);
    int pc = cdb[2] >> 6;
    int page = cdb[2] & 0x3f;
    uint8_t *buf;
    int pos = 4;

    if (pc == 3) {
        /* Saving parameters not supported */
        return md_check(r, MD_SK_ILLEGAL_REQUEST, 0x39, 0, 0, false, 0, NULL);
    }
    if (page != 0x3f && !md_page_valid(s, page)) {
        return md_invalid_field(r, 2);
    }

    buf = md_alloc_buf(r, 256);
    if (block_desc) {
        /* No block count, and a changeable block length */
        if (pc == 1) {
            buf[pos + 5] = 0xff;
            buf[pos + 6] = 0xff;
            buf[pos + 7] = 0xff;
        } else {
            buf[pos + 6] = MD_BLOCK_SIZE >> 8;
            buf[pos + 7] = MD_BLOCK_SIZE & 0xff;
        }
        pos += 8;
    }

    for (int i = 0; i < 0x3f; i++) {
        int len;

        if ((page != 0x3f && page != i) || !md_page_valid(s, i)) {
            continue;
        }
        len = s->model->pages[i][1];
        buf[pos++] = md_page_byte(s, pc, i, 0);
        buf[pos++] = len;
        for (int j = 0; j < len; j++) {
            buf[pos++] = (j + 2 < MD_PAGE_LEN) ? md_page_byte(s, pc, i, j + 2)
                                               : 0;
        }
    }

    buf[0] = pos - 1;
    buf[1] = md_present(s) ? 0x03 : 0x00;    /* Medium type: MD DATA */
    /* Write protected, also without a disc */
    buf[2] = (!md_present(s) || md_read_only(s)) ? 0x80 : 0x00;
    buf[3] = block_desc ? 8 : 0;

    return md_data_in(r, pos);
}

static int32_t md_mode_select(SCSIMDReq *r, SCSIMDState *s)
{
    const uint8_t *buf = r->buf;
    uint32_t len = MIN(r->req.cmd.buf[4], r->req.cmd.xfer);
    uint32_t block_desc_len, pos;
    uint8_t pages[MD_PAGES][MD_PAGE_LEN];

    if (len == 0) {
        scsi_req_complete(&r->req, GOOD);
        return 0;
    }
    if (len < 4) {
        return md_invalid_param_list(r);
    }

    block_desc_len = buf[3];
    pos = 4 + block_desc_len;
    if (pos > len) {
        return md_invalid_param_list(r);
    }

    /* The block length can only be the one of the disc */
    if (block_desc_len >= 8) {
        uint32_t block_len = (buf[9] << 16) | (buf[10] << 8) | buf[11];

        if (block_len != 0 && block_len != MD_BLOCK_SIZE) {
            return md_invalid_param_list(r);
        }
    }

    /* Nothing changes unless all of the list is good */
    memcpy(pages, s->pages, sizeof(pages));

    while (pos + 2 <= len) {
        /* The PS bit, which Sony's Mac OS driver sends back, is ignored */
        int page = buf[pos] & 0x3f;
        uint32_t page_len = buf[pos + 1];

        if (!md_page_valid(s, page) || pos + 2 + page_len > len) {
            return md_invalid_param_list(r);
        }

        for (uint32_t i = 0; i < page_len; i++) {
            uint8_t val = buf[pos + 2 + i];
            uint8_t old, ch;

            if (i + 2 >= MD_PAGE_LEN) {
                if (val) {
                    return md_invalid_param_list(r);
                }
                continue;
            }
            ch = md_pages_changeable[page][i + 2];
            old = pages[page][i + 2];
            if ((val ^ old) & ~ch) {
                return md_invalid_param_list(r);
            }
            pages[page][i + 2] = (old & ~ch) | (val & ch);
        }

        pos += 2 + page_len;
    }

    memcpy(s->pages, pages, sizeof(pages));
    scsi_req_complete(&r->req, GOOD);
    return 0;
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
    case 0xd6:  /* Stop audio playback */
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

    case MODE_SENSE:
        return md_mode_sense(r, s, cdb);

    case MODE_SELECT:
        return md_data_out(r);

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
            s->utoc_valid = false;
            trace_scsi_md_tray(false);
        }
        scsi_req_complete(&r->req, GOOD);
        return 0;

    case ALLOW_MEDIUM_REMOVAL:
        s->tray_locked = cdb[4] & 1;
        blk_lock_medium(s->qdev.conf.blk, s->tray_locked);
        scsi_req_complete(&r->req, GOOD);
        return 0;

    case 0xd1: /* Read U-TOC sectors */
        if (cdb[5] >= MD_UTOC_SECTORS) {
            return md_invalid_field(r, 5);
        }
        md_utoc_load(s);
        buf = md_alloc_buf(r, (MD_UTOC_SECTORS - cdb[5]) * MD_TOC_SECTOR_SIZE);
        memcpy(buf, s->utoc + cdb[5] * MD_TOC_SECTOR_SIZE,
               (MD_UTOC_SECTORS - cdb[5]) * MD_TOC_SECTOR_SIZE);
        return md_data_in(r, (MD_UTOC_SECTORS - cdb[5]) * MD_TOC_SECTOR_SIZE);

    case 0xd2: /* Write U-TOC sectors */
        if (md_read_only(s)) {
            return md_write_protected(r);
        }
        if (cdb[5] >= MD_UTOC_SECTORS) {
            return md_invalid_field(r, 5);
        }
        if (r->req.cmd.xfer == 0) {
            scsi_req_complete(&r->req, GOOD);
            return 0;
        }
        return md_data_out(r);

    case 0xd4: /* Read a P-TOC sector: recordable discs only have sector 0 */
        if (cdb[5] != 0) {
            return md_invalid_field(r, 5);
        }
        md_build_ptoc(md_alloc_buf(r, MD_TOC_SECTOR_SIZE));
        return md_data_in(r, MD_TOC_SECTOR_SIZE);

    case 0xd5: /* Result of the last format: no defect entries */
        md_alloc_buf(r, MAX(r->req.cmd.xfer, 4));
        return md_data_in(r, MAX(r->req.cmd.xfer, 4));

    case 0xd7: /* Audio play position: not playing */
        md_alloc_buf(r, 4);
        return md_data_in(r, 4);

    case 0xd8: /* Disc type: 01h and the first block of the data track */
        md_utoc_load(s);
        start = md_data_track_start(s);
        buf = md_alloc_buf(r, 8);
        if (start < 0) {
            buf[0] = 0xff;
        } else {
            buf[0] = 0x01;
            stl_be_p(buf + 4, start);
        }
        return md_data_in(r, 8);

    default:
        return md_check(r, MD_SK_ILLEGAL_REQUEST, 0x20, 0, 0, false, 0, NULL);
    }
}

/* Data from the host for an emulated command has arrived */
static void md_emulate_write_done(SCSIMDReq *r, SCSIMDState *s)
{
    const uint8_t *cdb = r->req.cmd.buf;
    uint32_t len;

    switch (cdb[0]) {
    case MODE_SELECT:
        md_mode_select(r, s);
        break;

    case 0xd2:
        len = MIN(r->req.cmd.xfer,
                  (MD_UTOC_SECTORS - cdb[5]) * MD_TOC_SECTOR_SIZE);
        md_utoc_load(s);
        memcpy(s->utoc + cdb[5] * MD_TOC_SECTOR_SIZE, r->buf, len);
        if (!md_utoc_save(s)) {
            md_check(r, MD_SK_MEDIUM_ERROR, 0x0c, 0, 0, false, 0, NULL);
            break;
        }
        scsi_req_complete(&r->req, GOOD);
        break;

    default:
        /* SEND DIAGNOSTIC and VERIFY with byte compare: accepted */
        scsi_req_complete(&r->req, GOOD);
        break;
    }
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

/*
 * The vendor commands (C0h to FFh) are 10 bytes long, with the
 * allocation or parameter list length in bytes 7 and 8.
 */
static int md_parse_cdb(SCSIDevice *dev, SCSICommand *cmd, uint8_t *buf,
                        size_t buf_len, void *hba_private)
{
    int ret;

    if (buf[0] >= 0xc0) {
        if (buf_len < 10) {
            return -1;
        }
        cmd->len = 10;
        cmd->xfer = lduw_be_p(&buf[7]);
        cmd->lba = -1;
        memcpy(cmd->buf, buf, 10);
        if (!cmd->xfer || buf[0] == 0xd6) {
            cmd->xfer = 0;
            cmd->mode = SCSI_XFER_NONE;
        } else {
            cmd->mode = buf[0] == 0xd2 ? SCSI_XFER_TO_DEV : SCSI_XFER_FROM_DEV;
        }
        return 0;
    }

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
    s->utoc_valid = false;
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
        s->utoc_valid = false;
    }
    s->tray_locked = false;
    memcpy(s->pages, s->model->pages, sizeof(s->pages));
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

    memcpy(s->pages, s->model->pages, sizeof(s->pages));
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
        VMSTATE_BOOL(utoc_valid, SCSIMDState),
        VMSTATE_BUFFER(utoc, SCSIMDState),
        VMSTATE_UINT8_2DARRAY(pages, SCSIMDState, MD_PAGES, MD_PAGE_LEN),
        VMSTATE_END_OF_LIST()
    }
};

static const Property scsi_md_properties[] = {
    DEFINE_PROP_DRIVE("drive", SCSIMDState, qdev.conf.blk),
    DEFINE_PROP_STRING("model", SCSIMDState, model_name),
    DEFINE_PROP_STRING("utoc", SCSIMDState, utoc_prop),
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
