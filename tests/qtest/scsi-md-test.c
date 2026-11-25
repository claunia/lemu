/*
 * QTest testcase for the Sony MD DATA drive (scsi-md) on virtio-scsi
 *
 * The expected responses are those of real drives, as in Cockatrice III's
 * test_mdh10.cpp and the MDH-10 bus captures.
 *
 * Copyright (c) 2026 Natalia Portillo
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest-single.h"
#include "qemu/module.h"
#include "qemu/bswap.h"
#include "scsi/constants.h"
#include "libqos/virtio.h"
#include "standard-headers/linux/virtio_scsi.h"
#include "libqos/virtio-scsi.h"
#include "libqos/qgraph.h"

#define QVIRTIO_SCSI_TIMEOUT_US (1 * 1000 * 1000)
#define MAX_NUM_QUEUES          64

#define MD_IMAGE_SIZE           (70464 * 2048)
#define MD_TOC_SECTOR_SIZE      2336

typedef struct {
    QVirtioDevice *dev;
    int num_queues;
    QVirtQueue *vq[MAX_NUM_QUEUES + 2];
} QVirtioSCSIQueues;

static QGuestAllocator *alloc;
static char *tmp_dir;
static char *image_path;
static char *utoc_path;

/* Runs one command; data_in gets what the drive returned */
static uint8_t md_cmd(QVirtioSCSIQueues *vs, const uint8_t *cdb, int cdb_len,
                      const uint8_t *data_out, size_t data_out_len,
                      uint8_t *data_in, size_t data_in_len,
                      struct virtio_scsi_cmd_resp *resp_out)
{
    QTestState *qts = global_qtest;
    QVirtQueue *vq = vs->vq[2];
    struct virtio_scsi_cmd_req req = { { 0 } };
    struct virtio_scsi_cmd_resp resp = { .response = 0xff, .status = 0xff };
    uint64_t req_addr, resp_addr, in_addr = 0, out_addr = 0;
    uint32_t free_head;
    uint8_t response;

    req.lun[0] = 1;
    req.lun[1] = 1;     /* Target 1 */
    memcpy(req.cdb, cdb, cdb_len);

    req_addr = guest_alloc(alloc, sizeof(req));
    memwrite(req_addr, &req, sizeof(req));
    free_head = qvirtqueue_add(qts, vq, req_addr, sizeof(req), false, true);

    if (data_out_len) {
        out_addr = guest_alloc(alloc, data_out_len);
        memwrite(out_addr, data_out, data_out_len);
        qvirtqueue_add(qts, vq, out_addr, data_out_len, false, true);
    }

    resp_addr = guest_alloc(alloc, sizeof(resp));
    memwrite(resp_addr, &resp, sizeof(resp));
    qvirtqueue_add(qts, vq, resp_addr, sizeof(resp), true, !!data_in_len);

    if (data_in_len) {
        in_addr = guest_alloc(alloc, data_in_len);
        qtest_memset(qts, in_addr, 0, data_in_len);
        qvirtqueue_add(qts, vq, in_addr, data_in_len, true, false);
    }

    qvirtqueue_kick(qts, vs->dev, vq, free_head);
    qvirtio_wait_used_elem(qts, vs->dev, vq, free_head, NULL,
                           QVIRTIO_SCSI_TIMEOUT_US);

    memread(resp_addr, &resp, sizeof(resp));
    response = resp.response;
    if (data_in_len) {
        memread(in_addr, data_in, data_in_len);
    }
    if (resp_out) {
        *resp_out = resp;
    }

    guest_free(alloc, req_addr);
    guest_free(alloc, resp_addr);
    guest_free(alloc, in_addr);
    guest_free(alloc, out_addr);
    return response;
}

/* Runs a command and checks its status */
static void md_expect(QVirtioSCSIQueues *vs, const uint8_t *cdb, int cdb_len,
                      const uint8_t *out, size_t out_len,
                      uint8_t *in, size_t in_len, uint8_t status,
                      struct virtio_scsi_cmd_resp *resp)
{
    struct virtio_scsi_cmd_resp r;

    g_assert_cmpint(md_cmd(vs, cdb, cdb_len, out, out_len, in, in_len, &r),
                    ==, 0);
    g_assert_cmphex(r.status, ==, status);
    if (resp) {
        *resp = r;
    }
}

/* in_len is the allocation length in the CDB */
static void md_expect_sense(QVirtioSCSIQueues *vs, const uint8_t *cdb,
                            int cdb_len, size_t in_len, uint8_t key,
                            uint8_t asc, uint8_t ascq)
{
    g_autofree uint8_t *in = g_malloc0(MAX(in_len, 1));
    struct virtio_scsi_cmd_resp r;

    md_expect(vs, cdb, cdb_len, NULL, 0, in, in_len, CHECK_CONDITION, &r);
    g_assert_cmphex(r.sense[2], ==, key);
    g_assert_cmphex(r.sense[12], ==, asc);
    g_assert_cmphex(r.sense[13], ==, ascq);
}

static QVirtioSCSIQueues *md_init(QVirtioDevice *dev)
{
    const uint8_t tur[6] = { 0 };
    QVirtioSCSIQueues *vs;
    struct virtio_scsi_cmd_resp r;
    uint64_t features;

    vs = g_new0(QVirtioSCSIQueues, 1);
    vs->dev = dev;

    features = qvirtio_get_features(dev);
    features &= ~(QVIRTIO_F_BAD_FEATURE | (1ull << VIRTIO_RING_F_EVENT_IDX));
    qvirtio_set_features(dev, features);

    vs->num_queues = qvirtio_config_readl(dev, 0);
    g_assert_cmpint(vs->num_queues, <, MAX_NUM_QUEUES);
    for (int i = 0; i < vs->num_queues + 2; i++) {
        vs->vq[i] = qvirtqueue_setup(dev, alloc, i);
    }
    qvirtio_set_driver_ok(dev);

    /* POWER ON from the bus, then the drive's own "medium may have changed" */
    md_expect_sense(vs, tur, 6, 0, UNIT_ATTENTION, 0x29, 0x00);
    md_expect(vs, tur, 6, NULL, 0, NULL, 0, CHECK_CONDITION, &r);
    g_assert_cmphex(r.sense[0], ==, 0x70);
    g_assert_cmphex(r.sense[2], ==, UNIT_ATTENTION);
    g_assert_cmphex(r.sense[12], ==, 0x28);
    g_assert_cmphex(r.sense[14], ==, 0xcc);
    md_expect(vs, tur, 6, NULL, 0, NULL, 0, GOOD, NULL);

    return vs;
}

static void md_free(QVirtioSCSIQueues *vs)
{
    for (int i = 0; i < vs->num_queues + 2; i++) {
        qvirtqueue_cleanup(vs->dev->bus, vs->vq[i], alloc);
    }
    g_free(vs);

    unlink(utoc_path);
    unlink(image_path);
    rmdir(tmp_dir);
    g_clear_pointer(&utoc_path, g_free);
    g_clear_pointer(&image_path, g_free);
    g_clear_pointer(&tmp_dir, g_free);
}

static void test_identify(void *obj, void *data, QGuestAllocator *t_alloc)
{
    QVirtioSCSI *scsi = obj;
    QVirtioSCSIQueues *vs;
    const uint8_t inquiry[6] = { 0x12, 0, 0, 0, 36, 0 };
    const uint8_t capacity[10] = { 0x25 };
    const uint8_t sense_all[6] = { 0x1a, 0, 0x3f, 0, 0xff, 0 };
    const uint8_t sense_01_dbd[6] = { 0x1a, 0x08, 0x01, 0, 4, 0 };
    const uint8_t sense_saved[6] = { 0x1a, 0, 0xff, 0, 0xff, 0 };
    const uint8_t ptoc[10] = { 0xd4, 0, 0, 0, 0, 0, 0, 0x09, 0x20, 0 };
    const uint8_t ptoc1[10] = { 0xd4, 0, 0, 0, 0, 1, 0, 0x09, 0x20, 0 };
    const uint8_t disc_type[10] = { 0xd8, 0, 1, 0, 0, 0, 0, 0, 8, 0 };
    uint8_t buf[MD_TOC_SECTOR_SIZE];
    struct virtio_scsi_cmd_resp r;

    alloc = t_alloc;
    vs = md_init(scsi->vdev);

    md_expect(vs, inquiry, 6, NULL, 0, buf, 36, GOOD, NULL);
    g_assert_cmphex(buf[0], ==, 0x10);
    g_assert_cmphex(buf[1], ==, 0x85);
    g_assert(!memcmp(buf + 8, "SONY    MDH-10          1.11", 28));

    md_expect(vs, capacity, 10, NULL, 0, buf, 8, GOOD, NULL);
    g_assert_cmphex(ldl_be_p(buf), ==, 70463);
    g_assert_cmphex(ldl_be_p(buf + 4), ==, 2048);

    /* Header, block descriptor, pages 01, 07, 08, 20 and 21 */
    md_expect(vs, sense_all, 6, NULL, 0, buf, 255, GOOD, NULL);
    g_assert_cmphex(buf[0], ==, 4 + 8 + 12 + 12 + 12 + 16 + 16 - 1);
    g_assert_cmphex(buf[1], ==, 0x03);
    g_assert_cmphex(buf[3], ==, 8);
    g_assert_cmphex(lduw_be_p(buf + 10), ==, 2048);
    g_assert_cmphex(buf[12], ==, 0x01);
    g_assert_cmphex(buf[14], ==, 0x80);

    /* As captured: 1a 08 01 00 04 00 gives 0f 03 00 00 */
    md_expect(vs, sense_01_dbd, 6, NULL, 0, buf, 4, GOOD, NULL);
    g_assert_cmphex(ldl_be_p(buf), ==, 0x0f030000);

    md_expect(vs, sense_saved, 6, NULL, 0, buf, 255, CHECK_CONDITION, &r);
    g_assert_cmphex(r.sense[12], ==, 0x39);

    md_expect(vs, ptoc, 10, NULL, 0, buf, MD_TOC_SECTOR_SIZE, GOOD, NULL);
    g_assert(!memcmp(buf + 8, "MINX", 4));
    g_assert_cmphex(lduw_be_p(buf + 0x10), ==, 2252);
    g_assert_cmphex(buf[0x1d], ==, 50);

    md_expect(vs, ptoc1, 10, NULL, 0, buf, MD_TOC_SECTOR_SIZE, CHECK_CONDITION,
              &r);
    g_assert_cmphex(r.sense[12], ==, 0x24);
    g_assert_cmphex(r.sense[15], ==, 0xc0);
    g_assert_cmphex(r.sense[17], ==, 5);

    /* A blank disc has no data track */
    md_expect(vs, disc_type, 10, NULL, 0, buf, 8, GOOD, NULL);
    g_assert_cmphex(buf[0], ==, 0xff);

    md_free(vs);
}

static void test_format_and_io(void *obj, void *data, QGuestAllocator *t_alloc)
{
    QVirtioSCSI *scsi = obj;
    QVirtioSCSIQueues *vs;
    const uint8_t format[6] = { 0x04 };
    const uint8_t disc_type[10] = { 0xd8, 0, 1, 0, 0, 0, 0, 0, 8, 0 };
    const uint8_t read_utoc[10] = { 0xd1, 0, 0, 0, 0, 0, 0, 0x09, 0x20, 0 };
    const uint8_t write_utoc[10] = { 0xd2, 0, 0, 0, 0, 0, 0, 0x09, 0x20, 0 };
    const uint8_t write10[10] = { 0x2a, 0, 0, 0, 0, 5, 0, 0, 2, 0x80 };
    const uint8_t read10[10] = { 0x28, 0, 0, 0, 0, 5, 0, 0, 2, 0 };
    const uint8_t read_end[10] = { 0x28, 0, 0, 1, 0x13, 0x40, 0, 0, 1, 0 };
    g_autofree uint8_t *pattern = g_malloc(2 * 2048);
    g_autofree uint8_t *back = g_malloc(2 * 2048);
    g_autofree gchar *saved = NULL;
    uint8_t utoc[MD_TOC_SECTOR_SIZE];
    struct virtio_scsi_cmd_resp r;
    gsize saved_len;

    alloc = t_alloc;
    vs = md_init(scsi->vdev);

    md_expect(vs, format, 6, NULL, 0, NULL, 0, GOOD, NULL);

    /* The drive writes the MD DATA U-TOC itself */
    md_expect(vs, disc_type, 10, NULL, 0, utoc, 8, GOOD, NULL);
    g_assert_cmphex(ldl_be_p(utoc), ==, 0x01000000);
    g_assert_cmphex(ldl_be_p(utoc + 4), ==, 0x400);

    md_expect(vs, read_utoc, 10, NULL, 0, utoc, MD_TOC_SECTOR_SIZE, GOOD, NULL);
    g_assert_cmphex(utoc[0x0d], ==, 0x81);
    g_assert_cmphex(ldl_be_p(utoc + 0x128), ==, 0x014800d0);
    g_assert_cmphex(ldl_be_p(utoc + 0x130), ==, 0x00c800dc);

    g_assert(g_file_get_contents(utoc_path, &saved, &saved_len, NULL));
    g_assert_cmpint(saved_len, ==, MD_TOC_SECTOR_SIZE);
    g_assert(!memcmp(saved, utoc, MD_TOC_SECTOR_SIZE));

    /* U-TOC written back */
    utoc[0x30] = 0x5a;
    md_expect(vs, write_utoc, 10, utoc, MD_TOC_SECTOR_SIZE, NULL, 0, GOOD,
              NULL);
    memset(utoc, 0, sizeof(utoc));
    md_expect(vs, read_utoc, 10, NULL, 0, utoc, MD_TOC_SECTOR_SIZE, GOOD, NULL);
    g_assert_cmphex(utoc[0x30], ==, 0x5a);

    /* Blocks written and read back */
    for (int i = 0; i < 2 * 2048; i++) {
        pattern[i] = i * 7 + 3;
    }
    md_expect(vs, write10, 10, pattern, 2 * 2048, NULL, 0, GOOD, NULL);
    md_expect(vs, read10, 10, NULL, 0, back, 2 * 2048, GOOD, NULL);
    g_assert(!memcmp(pattern, back, 2 * 2048));

    /* Past the end of the disc */
    md_expect(vs, read_end, 10, NULL, 0, back, 2048, CHECK_CONDITION, &r);
    g_assert_cmphex(r.sense[0], ==, 0xf0);
    g_assert_cmphex(r.sense[2], ==, ILLEGAL_REQUEST);
    g_assert_cmphex(r.sense[12], ==, 0x21);
    g_assert_cmphex(ldl_be_p(r.sense + 3), ==, 70464);

    md_free(vs);
}

static void test_eject(void *obj, void *data, QGuestAllocator *t_alloc)
{
    QVirtioSCSI *scsi = obj;
    QVirtioSCSIQueues *vs;
    const uint8_t tur[6] = { 0 };
    const uint8_t prevent[6] = { 0x1e, 0, 0, 0, 1, 0 };
    const uint8_t allow[6] = { 0x1e, 0, 0, 0, 0, 0 };
    const uint8_t eject[6] = { 0x1b, 0, 0, 0, 2, 0 };
    const uint8_t load[6] = { 0x1b, 0, 0, 0, 3, 0 };
    const uint8_t mode_sense[6] = { 0x1a, 0x08, 0x3f, 0, 4, 0 };
    uint8_t buf[4];

    alloc = t_alloc;
    vs = md_init(scsi->vdev);

    md_expect(vs, prevent, 6, NULL, 0, NULL, 0, GOOD, NULL);
    md_expect_sense(vs, eject, 6, 0, ILLEGAL_REQUEST, 0x53, 0x02);
    md_expect(vs, allow, 6, NULL, 0, NULL, 0, GOOD, NULL);
    md_expect(vs, eject, 6, NULL, 0, NULL, 0, GOOD, NULL);
    md_expect_sense(vs, tur, 6, 0, NOT_READY, 0x3a, 0x00);

    /* Without a disc: no medium type, write protected */
    md_expect(vs, mode_sense, 6, NULL, 0, buf, 4, GOOD, NULL);
    g_assert_cmphex(buf[1], ==, 0x00);
    g_assert_cmphex(buf[2], ==, 0x80);

    md_expect(vs, load, 6, NULL, 0, NULL, 0, GOOD, NULL);
    md_expect_sense(vs, tur, 6, 0, UNIT_ATTENTION, 0x28, 0x00);
    md_expect(vs, tur, 6, NULL, 0, NULL, 0, GOOD, NULL);

    md_free(vs);
}

static void test_mdm110(void *obj, void *data, QGuestAllocator *t_alloc)
{
    QVirtioSCSI *scsi = obj;
    QVirtioSCSIQueues *vs;
    const uint8_t inquiry[6] = { 0x12, 0, 0, 0, 36, 0 };
    const uint8_t sense_21[6] = { 0x1a, 0x08, 0x21, 0, 0xff, 0 };
    uint8_t buf[36];

    alloc = t_alloc;
    vs = md_init(scsi->vdev);

    md_expect(vs, inquiry, 6, NULL, 0, buf, 36, GOOD, NULL);
    g_assert(!memcmp(buf + 8, "SONY    MDM110          1.01", 28));

    /* The MDM-110 has no audio volume page */
    md_expect_sense(vs, sense_21, 6, 255, ILLEGAL_REQUEST, 0x24, 0x00);

    md_free(vs);
}

static void md_make_image(void)
{
    int fd;

    tmp_dir = g_dir_make_tmp("scsi-md-test-XXXXXX", NULL);
    g_assert(tmp_dir);
    image_path = g_build_filename(tmp_dir, "disc.img", NULL);
    utoc_path = g_build_filename(tmp_dir, "disc.utoc.bin", NULL);

    fd = open(image_path, O_CREAT | O_TRUNC | O_RDWR, 0600);
    g_assert(fd >= 0);
    g_assert(ftruncate(fd, MD_IMAGE_SIZE) == 0);
    close(fd);
    unlink(utoc_path);
}

static void *md_setup(GString *cmd_line, void *arg)
{
    md_make_image();
    g_string_append_printf(cmd_line,
                           " -drive file=%s,if=none,id=md0,format=raw"
                           " -device scsi-md,drive=md0,lun=0,scsi-id=1%s",
                           image_path, arg ? (const char *)arg : "");
    return NULL;
}

static void register_scsi_md_test(void)
{
    QOSGraphTestOptions opts = { };

    opts.before = md_setup;
    qos_add_test("scsi-md/identify", "virtio-scsi", test_identify, &opts);
    qos_add_test("scsi-md/format-and-io", "virtio-scsi", test_format_and_io,
                 &opts);
    qos_add_test("scsi-md/eject", "virtio-scsi", test_eject, &opts);

    opts.arg = (void *)",model=mdm110";
    qos_add_test("scsi-md/mdm110", "virtio-scsi", test_mdm110, &opts);
}

libqos_init(register_scsi_md_test);
