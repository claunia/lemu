/*
 * QEMU ATAPI CD-ROM Emulator
 *
 * Copyright (c) 2006 Fabrice Bellard
 * Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

/* ??? Most of the ATAPI emulation is still in ide.c.  It should be moved
   here.  */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "hw/scsi/scsi.h"
#include "qapi/error.h"
#include "qemu/cutils.h"

static void lba_to_msf(uint8_t *buf, int lba)
{
    lba += 150;
    buf[0] = (lba / 75) / 60;
    buf[1] = (lba / 75) % 60;
    buf[2] = lba % 75;
}

static uint8_t *toc_address(uint8_t *q, int msf, int lba)
{
    if (msf) {
        *q++ = 0; /* reserved */
        lba_to_msf(q, lba);
        q += 3;
    } else {
        stl_be_p(q, lba);
        q += 4;
    }
    return q;
}

/*
 * TOC of a one-session data CD whose tracks start at the given blocks
 * (the first at 0). Return -1 if error or the toc length.
 */
int cdrom_read_toc_tracks(int nb_sectors, uint8_t *buf, int msf,
                          int start_track, const uint32_t *starts,
                          int ntracks)
{
    uint8_t *q;
    int len, t;

    if (start_track > ntracks && start_track != 0xaa) {
        return -1;
    }
    q = buf + 2;
    *q++ = 1;       /* first track */
    *q++ = ntracks; /* last track */
    for (t = 1; t <= ntracks; t++) {
        if (t < start_track || start_track == 0xaa) {
            continue;
        }
        *q++ = 0; /* reserved */
        *q++ = 0x14; /* ADR, control */
        *q++ = t;    /* track number */
        *q++ = 0; /* reserved */
        q = toc_address(q, msf, starts[t - 1]);
    }
    /* lead out track */
    *q++ = 0; /* reserved */
    *q++ = 0x16; /* ADR, control */
    *q++ = 0xaa; /* track number */
    *q++ = 0; /* reserved */
    q = toc_address(q, msf, nb_sectors);
    len = q - buf;
    stw_be_p(buf, len - 2);
    return len;
}

/* same toc as bochs */
int cdrom_read_toc(int nb_sectors, uint8_t *buf, int msf, int start_track)
{
    static const uint32_t start;

    return cdrom_read_toc_tracks(nb_sectors, buf, msf, start_track, &start, 1);
}

static uint8_t *toc_raw_entry(uint8_t *q, int point, int msf, int lba)
{
    *q++ = 1; /* session number */
    *q++ = 0x14; /* data track */
    *q++ = 0; /* track number */
    *q++ = point;
    *q++ = 0; /* min */
    *q++ = 0; /* sec */
    *q++ = 0; /* frame */
    return toc_address(q, msf, lba);
}

static uint8_t *toc_raw_track_entry(uint8_t *q, int point, int track)
{
    *q++ = 1; /* session number */
    *q++ = 0x14; /* data track */
    *q++ = 0; /* track number */
    *q++ = point;
    *q++ = 0; /* min */
    *q++ = 0; /* sec */
    *q++ = 0; /* frame */
    *q++ = 0;
    *q++ = track;
    *q++ = 0x00; /* disk type */
    *q++ = 0x00;
    return q;
}

/* mostly same info as PearPc */
int cdrom_read_toc_raw_tracks(int nb_sectors, uint8_t *buf, int msf,
                              int session_num, const uint32_t *starts,
                              int ntracks)
{
    uint8_t *q;
    int len, t;

    q = buf + 2;
    *q++ = 1; /* first session */
    *q++ = 1; /* last session */

    q = toc_raw_track_entry(q, 0xa0, 1);        /* first track */
    q = toc_raw_track_entry(q, 0xa1, ntracks);  /* last track */
    q = toc_raw_entry(q, 0xa2, msf, nb_sectors); /* lead-out */
    for (t = 1; t <= ntracks; t++) {
        q = toc_raw_entry(q, t, msf, starts[t - 1]);
    }

    len = q - buf;
    stw_be_p(buf, len - 2);
    return len;
}

int cdrom_read_toc_raw(int nb_sectors, uint8_t *buf, int msf, int session_num)
{
    static const uint32_t start;

    return cdrom_read_toc_raw_tracks(nb_sectors, buf, msf, session_num,
                                     &start, 1);
}

/*
 * A CD image made of several data tracks, all MODE1 in one session, such
 * as a BeOS CD: "0:3402:160186" gives the block where each track starts.
 */
bool cdrom_parse_tracks(const char *tracks, uint32_t **starts, int *ntracks,
                        Error **errp)
{
    g_auto(GStrv) parts = g_strsplit(tracks, ":", 0);
    int n = g_strv_length(parts);
    g_autofree uint32_t *v = NULL;
    int i;

    if (n < 1 || n > 99) {
        error_setg(errp, "tracks: give 1 to 99 track starts");
        return false;
    }
    v = g_new(uint32_t, n);
    for (i = 0; i < n; i++) {
        uint64_t start;

        if (qemu_strtou64(parts[i], NULL, 0, &start) < 0 ||
            start > UINT32_MAX || (i == 0 && start != 0) ||
            (i > 0 && start <= v[i - 1])) {
            error_setg(errp, "tracks: '%s' must be increasing block numbers, "
                       "the first 0", tracks);
            return false;
        }
        v[i] = start;
    }
    g_free(*starts);
    *starts = g_steal_pointer(&v);
    *ntracks = n;
    return true;
}
