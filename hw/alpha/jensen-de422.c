/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Jensen's EISA DEC DE422-SA EtherWORKS Ethernet controller.
 *
 * An AMD Am7990 LANCE with 64 KB of board RAM. The LANCE sees the RAM at
 * address 0 and decodes only the low 16 address bits. The host sees it in
 * the EISA memory space at C0000, D0000 or E0000, or only its upper (or,
 * with BS, lower) 32 KB at C8000, D8000 or E8000. The console writes the
 * window and the IRQ into the configuration register from the ECU data
 * (!DEC4220.CFG).
 *
 * Slot I/O space 0xzC00:
 *   0x0  NICSR  network interface control and status
 *   0x4  RDP    LANCE register data port
 *   0x6  RAP    LANCE register address port
 *   0xC  read:  Ethernet address PROM, 32 bytes, one byte per read, rotating
 *        write: configuration register (write only)
 * and the EISA ID DEC4220 at 0xzC80.
 *
 * Register bits: Linux drivers/net/depca.h and NetBSD sys/dev/ic/depcareg.h.
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "hw/isa/isa.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "net/net.h"
#include "net/eth.h"
#include "hw/net/pcnet.h"
#include "migration/vmstate.h"
#include "trace.h"

#define TYPE_JENSEN_DE422 "jensen-de422"
OBJECT_DECLARE_SIMPLE_TYPE(JensenDE422, JENSEN_DE422)

#define DE422_RAM_SIZE  0x10000

#define NICSR_LED       0x0001
#define NICSR_IEN       0x0002  /* interrupt output enabled */
#define NICSR_IM        0x0004  /* interrupt masked */
#define NICSR_128KB     0x0008
#define NICSR_RBE       0x0010  /* remote boot enabled */
#define NICSR_BUF       0x0020  /* 32 KB window (from CNFG_BUFF) */
#define NICSR_BS        0x0040  /* 32 KB window shows the lower half */
#define NICSR_SHE       0x0080  /* shared memory enabled */
#define NICSR_TO        0x0100

#define CNFG_PADR17     0x0001  /* window at E0000 */
#define CNFG_PADR16     0x0002  /* window at D0000 */
#define CNFG_BUFF       0x0004  /* 32 KB window */
#define CNFG_IRQ5       0x0008
#define CNFG_IRQ9       0x0010
#define CNFG_IRQ10      0x0020
#define CNFG_IRQ11      0x0040
#define CNFG_MASK       0x01ff

#define EISA_CTRL_ENABLE 0x01
#define EISA_CTRL_RESET  0x04

struct JensenDE422 {
    ISADevice parent_obj;

    PCNetState lance;
    MemoryRegion io, eisa, ram, win64, win32[2];
    MemoryRegion *window;       /* the window mapped now, or NULL */
    hwaddr window_base;
    qemu_irq irq;               /* the IRQ the configuration register picks */
    uint32_t slot;
    uint16_t nicsr, cnfg, rbi, hbase, rbsa;
    uint8_t ctrl;
    uint8_t prom[32];
    uint8_t prom_pos;
    bool lance_irq;
};

static void de422_update_window(JensenDE422 *s)
{
    MemoryRegion *mem = isa_address_space(ISA_DEVICE(s));
    MemoryRegion *want = NULL;
    hwaddr base = 0xc0000;

    if (s->nicsr & NICSR_SHE) {
        base += (s->cnfg & CNFG_PADR16 ? 0x10000 : 0) +
                (s->cnfg & CNFG_PADR17 ? 0x20000 : 0);
        if (s->cnfg & CNFG_BUFF) {
            base += 0x8000;
            want = &s->win32[s->nicsr & NICSR_BS ? 0 : 1];
        } else {
            want = &s->win64;
        }
    }
    if (want == s->window && (!want || base == s->window_base)) {
        return;
    }
    if (s->window) {
        memory_region_del_subregion(mem, s->window);
    }
    s->window = want;
    s->window_base = base;
    if (want) {
        memory_region_add_subregion_overlap(mem, base, want, 1);
    }
}

static void de422_update_irq(JensenDE422 *s)
{
    if (s->irq) {
        qemu_set_irq(s->irq, s->lance_irq && (s->nicsr & NICSR_IEN) &&
                             !(s->nicsr & NICSR_IM));
    }
}

static void de422_lance_irq(void *opaque, int n, int level)
{
    JensenDE422 *s = opaque;

    s->lance_irq = level;
    de422_update_irq(s);
}

static void de422_set_nicsr(JensenDE422 *s, uint16_t value)
{
    if ((value & NICSR_128KB) && !(s->nicsr & NICSR_128KB)) {
        qemu_log_mask(LOG_UNIMP, "de422: 128 KB RAM mode not emulated\n");
    }
    s->nicsr = value & ~NICSR_BUF & 0x1ff;
    de422_update_window(s);
    de422_update_irq(s);
}

static void de422_set_cnfg(JensenDE422 *s, uint16_t value)
{
    static const struct {
        uint16_t bit;
        unsigned irq;
    } irqs[] = {
        { CNFG_IRQ5, 5 }, { CNFG_IRQ9, 9 }, { CNFG_IRQ10, 10 }, { CNFG_IRQ11, 11 },
    };
    qemu_irq irq = NULL;

    s->cnfg = value & CNFG_MASK;
    for (unsigned i = 0; i < ARRAY_SIZE(irqs); i++) {
        if (s->cnfg & irqs[i].bit) {
            irq = isa_get_irq(ISA_DEVICE(s), irqs[i].irq);
            break;
        }
    }
    if (irq != s->irq && s->irq) {
        qemu_set_irq(s->irq, 0);
    }
    s->irq = irq;
    de422_update_window(s);
    de422_update_irq(s);
}

static uint16_t de422_nicsr(JensenDE422 *s)
{
    return s->nicsr | (s->cnfg & CNFG_BUFF ? NICSR_BUF : 0);
}

/*
 * DEC's address PROM: the address and its checksum, the same eight bytes
 * reversed, again forward, then the test pattern that drivers look for.
 */
static void de422_build_prom(JensenDE422 *s)
{
    static const uint8_t pattern[8] = {
        0xff, 0x00, 0x55, 0xaa, 0xff, 0x00, 0x55, 0xaa
    };
    const uint8_t *mac = s->lance.conf.macaddr.a;
    uint32_t k = 0;

    for (unsigned j = 0; j < 3; j++) {
        k <<= 1;
        if (k > 0xffff) {
            k -= 0xffff;
        }
        k += mac[2 * j] | (mac[2 * j + 1] << 8);
        if (k > 0xffff) {
            k -= 0xffff;
        }
    }
    if (k == 0xffff) {
        k = 0;
    }
    memcpy(s->prom, mac, 6);
    s->prom[6] = k;
    s->prom[7] = k >> 8;
    for (unsigned i = 0; i < 8; i++) {
        s->prom[8 + i] = s->prom[7 - i];
    }
    memcpy(s->prom + 16, s->prom, 8);
    memcpy(s->prom + 24, pattern, sizeof(pattern));
}

static void de422_board_reset(JensenDE422 *s)
{
    pcnet_h_reset(&s->lance);
    s->nicsr = 0;
    s->rbi = s->hbase = s->rbsa = 0;
    s->prom_pos = 0;
    de422_set_cnfg(s, 0);
}

static uint64_t de422_io_read(void *opaque, hwaddr addr, unsigned size)
{
    JensenDE422 *s = opaque;
    uint64_t value;

    switch (addr) {
    case 0x0:
        value = de422_nicsr(s);
        break;
    case 0x1:
        value = de422_nicsr(s) >> 8;
        break;
    case 0x2:
        value = s->rbi;
        break;
    case 0x4:
        value = pcnet_ioport_readw(&s->lance, 0);
        break;
    case 0x6:
        value = pcnet_ioport_readw(&s->lance, 2);
        break;
    case 0x8:
        value = s->hbase;
        break;
    case 0xc:
        value = s->prom[s->prom_pos];
        s->prom_pos = (s->prom_pos + 1) % sizeof(s->prom);
        break;
    case 0xe:
        value = s->rbsa;
        break;
    default:
        value = 0xffff;
        break;
    }
    value &= size == 1 ? 0xff : 0xffff;
    trace_jensen_de422_read(addr, value, size);
    return value;
}

static void de422_io_write(void *opaque, hwaddr addr, uint64_t value,
                           unsigned size)
{
    JensenDE422 *s = opaque;

    trace_jensen_de422_write(addr, value, size);
    switch (addr) {
    case 0x0:
        de422_set_nicsr(s, size == 1 ? (s->nicsr & 0xff00) | value : value);
        break;
    case 0x1:
        de422_set_nicsr(s, (s->nicsr & 0xff) | (value << 8));
        break;
    case 0x2:
        s->rbi = value;
        break;
    case 0x4:
        pcnet_ioport_writew(&s->lance, 0, value);
        break;
    case 0x6:
        pcnet_ioport_writew(&s->lance, 2, value);
        break;
    case 0x8:
        s->hbase = value;
        break;
    case 0xc:
        de422_set_cnfg(s, size == 1 ? (s->cnfg & 0xff00) | value : value);
        break;
    case 0xd:
        de422_set_cnfg(s, (s->cnfg & 0xff) | (value << 8));
        break;
    case 0xe:
        s->rbsa = value;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "de422: write 0x%" PRIx64
                      " to unknown register 0x%" HWADDR_PRIx "\n", value, addr);
        break;
    }
}

static const MemoryRegionOps de422_io_ops = {
    .read = de422_io_read,
    .write = de422_io_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 2 },
    .impl = { .min_access_size = 1, .max_access_size = 2 },
};

static uint64_t de422_eisa_read(void *opaque, hwaddr addr, unsigned size)
{
    static const uint8_t id[] = { 0x10, 0xa3, 0x42, 0x20 }; /* DEC4220 */
    JensenDE422 *s = opaque;
    uint64_t value = 0;

    for (unsigned i = 0; i < size; i++) {
        unsigned reg = addr + i;
        uint8_t byte = reg < sizeof(id) ? id[reg] : reg == 4 ? s->ctrl : 0;

        value |= (uint64_t)byte << (i * 8);
    }
    return value;
}

static void de422_eisa_write(void *opaque, hwaddr addr, uint64_t value,
                             unsigned size)
{
    JensenDE422 *s = opaque;

    for (unsigned i = 0; i < size; i++) {
        if (addr + i == 4) {
            s->ctrl = value >> (i * 8);
            if (s->ctrl & EISA_CTRL_RESET) {
                de422_board_reset(s);
            }
        }
    }
}

static const MemoryRegionOps de422_eisa_ops = {
    .read = de422_eisa_read,
    .write = de422_eisa_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4, .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 4, .unaligned = true },
};

static void de422_trace_dma(bool write, hwaddr addr, const uint8_t *buf,
                            int len)
{
    if (trace_event_get_state_backends(TRACE_JENSEN_DE422_DMA)) {
        uint64_t data = 0;

        memcpy(&data, buf, MIN(len, 8));
        trace_jensen_de422_dma(write, addr & (DE422_RAM_SIZE - 1), len,
                               le64_to_cpu(data));
    }
}

/* The LANCE's view of memory: the board RAM, low 16 address bits. */
static void de422_ram_read(void *opaque, hwaddr addr, uint8_t *buf, int len,
                           int do_bswap)
{
    JensenDE422 *s = JENSEN_DE422(opaque);
    const uint8_t *ram = memory_region_get_ram_ptr(&s->ram);

    for (int i = 0; i < len; i++) {
        buf[i] = ram[(addr + i) & (DE422_RAM_SIZE - 1)];
    }
    de422_trace_dma(false, addr, buf, len);
}

static void de422_ram_write(void *opaque, hwaddr addr, uint8_t *buf, int len,
                            int do_bswap)
{
    JensenDE422 *s = JENSEN_DE422(opaque);
    uint8_t *ram = memory_region_get_ram_ptr(&s->ram);

    de422_trace_dma(true, addr, buf, len);
    for (int i = 0; i < len; i++) {
        ram[(addr + i) & (DE422_RAM_SIZE - 1)] = buf[i];
    }
    memory_region_set_dirty(&s->ram, 0, DE422_RAM_SIZE);
}

/*
 * The LANCE does not pad frames it loops back to itself; the self-test
 * sends 8-byte ones into 12-byte buffers. The NIC is set up so that QEMU
 * does not pad them, and frames from the network are padded here instead,
 * as they come on a wire.
 */
static ssize_t de422_receive(NetClientState *nc, const uint8_t *buf,
                             size_t size)
{
    PCNetState *s = qemu_get_nic_opaque(nc);
    uint8_t min_pkt[ETH_ZLEN];
    size_t min_pktsz = sizeof(min_pkt);

    if (!s->looptest && eth_pad_short_frame(min_pkt, &min_pktsz, buf, size)) {
        buf = min_pkt;
        size = min_pktsz;
    }
    return pcnet_receive(nc, buf, size);
}

static NetClientInfo de422_net_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .receive = de422_receive,
    .link_status_changed = pcnet_set_link_status,
};

static void de422_reset(DeviceState *dev)
{
    JensenDE422 *s = JENSEN_DE422(dev);

    s->ctrl = 0;
    de422_board_reset(s);
}

static void de422_realize(DeviceState *dev, Error **errp)
{
    JensenDE422 *s = JENSEN_DE422(dev);
    ISADevice *isa = ISA_DEVICE(dev);

    if (s->slot < 1 || s->slot > 6) {
        error_setg(errp, "DE422 requires EISA slot 1..6");
        return;
    }
    if (!memory_region_init_ram(&s->ram, OBJECT(s), "de422-ram",
                                DE422_RAM_SIZE, errp)) {
        return;
    }
    memory_region_init_alias(&s->win64, OBJECT(s), "de422-window", &s->ram,
                             0, 0x10000);
    memory_region_init_alias(&s->win32[0], OBJECT(s), "de422-window-low",
                             &s->ram, 0, 0x8000);
    memory_region_init_alias(&s->win32[1], OBJECT(s), "de422-window-high",
                             &s->ram, 0x8000, 0x8000);
    memory_region_init_io(&s->io, OBJECT(s), &de422_io_ops, s, "de422-io",
                          0x10);
    isa_register_ioport(isa, &s->io, (s->slot << 12) | 0xc00);
    memory_region_init_io(&s->eisa, OBJECT(s), &de422_eisa_ops, s,
                          "de422-eisa", 8);
    isa_register_ioport(isa, &s->eisa, (s->slot << 12) | 0xc80);

    s->lance.irq = qemu_allocate_irq(de422_lance_irq, s, 0);
    s->lance.phys_mem_read = de422_ram_read;
    s->lance.phys_mem_write = de422_ram_write;
    s->lance.dma_opaque = dev;
    s->lance.stop_clears_txon = true;
    pcnet_common_init(dev, &s->lance, &de422_net_info);
    qemu_get_queue(s->lance.nic)->do_not_pad = true;
    de422_build_prom(s);
}

static const Property de422_properties[] = {
    DEFINE_PROP_UINT32("slot", JensenDE422, slot, 5),
    DEFINE_NIC_PROPERTIES(JensenDE422, lance.conf),
};

static const VMStateDescription de422_vmstate = {
    .name = TYPE_JENSEN_DE422,
    .unmigratable = true,
};

static void de422_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = de422_realize;
    dc->vmsd = &de422_vmstate;
    device_class_set_legacy_reset(dc, de422_reset);
    device_class_set_props(dc, de422_properties);
    dc->desc = "Jensen EISA DEC DE422 EtherWORKS (LANCE)";
    set_bit(DEVICE_CATEGORY_NETWORK, dc->categories);
}

static const TypeInfo de422_type_info = {
    .name = TYPE_JENSEN_DE422,
    .parent = TYPE_ISA_DEVICE,
    .instance_size = sizeof(JensenDE422),
    .class_init = de422_class_init,
};

static void de422_register_types(void)
{
    type_register_static(&de422_type_info);
}

type_init(de422_register_types)
