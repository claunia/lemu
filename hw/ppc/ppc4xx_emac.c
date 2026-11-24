/*
 * IBM/AMCC PowerPC 4xx Ethernet MAC (EMAC) and ZMII bridge
 *
 * Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * The 10/100 EMAC of the 405 and 440GP/EP/GR. Frames move through the
 * MAL: TMR0's GNP bits make the MAL send the ready descriptors of the
 * EMAC's transmit channel, and received frames go into the empty
 * descriptors of its receive channel. STACR reaches the PHY over MDIO;
 * each EMAC has one PHY, at the address given by the "phy-addr" property,
 * with a 100 Mbit/s full duplex link. The ZMII bridge in front of the
 * EMACs only keeps its registers.
 *
 * Register layout and bits after the PowerPC 440EP user's manual as the
 * Linux (drivers/net/ethernet/ibm/emac) and U-Boot (ppc4xx_enet.h) drivers
 * use them.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/ppc/ppc4xx.h"
#include "net/net.h"
#include "net/eth.h"
#include "migration/vmstate.h"
#include "trace.h"
#include <zlib.h>

OBJECT_DECLARE_SIMPLE_TYPE(Ppc4xxEmacState, PPC4XX_EMAC)

OBJECT_DECLARE_SIMPLE_TYPE(Ppc4xxZmiiState, PPC4XX_ZMII)

#define EMAC_REGS_SIZE      0x70
#define EMAC_MAX_FRAME      1536

/* registers, as offsets / 4 */
#define R_MR0       (0x00 / 4)
#define R_MR1       (0x04 / 4)
#define R_TMR0      (0x08 / 4)
#define R_TMR1      (0x0c / 4)
#define R_RMR       (0x10 / 4)
#define R_ISR       (0x14 / 4)
#define R_ISER      (0x18 / 4)
#define R_IAHR      (0x1c / 4)
#define R_IALR      (0x20 / 4)
#define R_GAHT1     (0x40 / 4)
#define R_STACR     (0x5c / 4)
#define R_OCTX      (0x68 / 4)
#define R_OCRX      (0x6c / 4)

#define MR0_RXI     0x80000000
#define MR0_TXI     0x40000000
#define MR0_SRST    0x20000000
#define MR0_TXE     0x10000000
#define MR0_RXE     0x08000000

#define TMR0_GNP0   0x80000000
#define TMR0_GNP1   0x40000000

#define RMR_SFCS    0x40000000
#define RMR_PME     0x01000000
#define RMR_PMME    0x00800000
#define RMR_IAE     0x00400000
#define RMR_MIAE    0x00200000
#define RMR_BAE     0x00100000
#define RMR_MAE     0x00080000

#define STACR_PHYD_SHIFT    16
#define STACR_OC            0x00008000
#define STACR_PHYE          0x00004000
#define STACR_WRITE         0x00002000
#define STACR_READ          0x00001000
#define STACR_PCDA(v)       (((v) >> 5) & 0x1f)
#define STACR_PRA(v)        ((v) & 0x1f)

struct Ppc4xxEmacState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    NICConf conf;
    NICState *nic;
    qemu_irq irq;
    Ppc4xxMalState *mal;
    uint32_t tx_chan;           /* the first of the two transmit channels */
    uint32_t rx_chan;
    uint32_t phy_addr;

    uint32_t regs[EMAC_REGS_SIZE / 4];
    uint16_t phy[32];
};

/* PHY */

static void emac_phy_reset(Ppc4xxEmacState *s)
{
    memset(s->phy, 0, sizeof(s->phy));
    s->phy[0] = 0x3100;     /* 100 Mbit/s, autonegotiation, full duplex */
    s->phy[1] = 0x782d;     /* link up, autonegotiation complete */
    s->phy[2] = 0x0181;     /* Davicom DM9161 */
    s->phy[3] = 0xb8a0;
    s->phy[4] = 0x01e1;
    s->phy[5] = 0x45e1;     /* the link partner does 10 and 100 full */
    s->phy[6] = 0x0001;
}

static void emac_mdio(Ppc4xxEmacState *s, uint32_t val)
{
    int phy = STACR_PCDA(val), reg = STACR_PRA(val);
    bool ours = phy == s->phy_addr;
    uint32_t data = val >> STACR_PHYD_SHIFT;

    if (val & STACR_READ) {
        data = ours ? s->phy[reg] : 0xffff;
        trace_ppc4xx_emac_mdio_read(phy, reg, data);
    } else if (val & STACR_WRITE) {
        trace_ppc4xx_emac_mdio_write(phy, reg, data);
        if (ours) {
            switch (reg) {
            case 0:
                if (data & 0x8000) {
                    emac_phy_reset(s);
                } else {
                    /* restarting negotiation finishes at once */
                    s->phy[0] = data & ~0x0200;
                }
                break;
            case 4:
                s->phy[4] = data;
                break;
            }
        }
    }
    /* the operation is done at once */
    s->regs[R_STACR] = (data << STACR_PHYD_SHIFT) | (val & 0x0fff) |
                       STACR_OC;
}

/* EMAC */

static void emac_update_irq(Ppc4xxEmacState *s)
{
    qemu_set_irq(s->irq, !!(s->regs[R_ISR] & s->regs[R_ISER]));
}

static void emac_reset_regs(Ppc4xxEmacState *s)
{
    memset(s->regs, 0, sizeof(s->regs));
    s->regs[R_STACR] = STACR_OC;
    emac_update_irq(s);
}

static void emac_send(void *opaque, const uint8_t *buf, size_t len)
{
    Ppc4xxEmacState *s = opaque;

    trace_ppc4xx_emac_tx(s->tx_chan, len);
    if (s->nic) {
        qemu_send_packet(qemu_get_queue(s->nic), buf, len);
    }
    s->regs[R_OCTX] += len;
}

static void emac_transmit(Ppc4xxEmacState *s, uint32_t gnp)
{
    if (!(s->regs[R_MR0] & MR0_TXE)) {
        return;
    }
    if (gnp & TMR0_GNP0) {
        ppc4xx_mal_transmit(s->mal, s->tx_chan, emac_send, s);
    }
    if (gnp & TMR0_GNP1) {
        ppc4xx_mal_transmit(s->mal, s->tx_chan + 1, emac_send, s);
    }
}

static bool emac_rx_accept(Ppc4xxEmacState *s, const uint8_t *buf)
{
    uint32_t rmr = s->regs[R_RMR];
    uint8_t ia[6];

    if (rmr & RMR_PME) {
        return true;
    }
    if (is_broadcast_ether_addr(buf)) {
        return rmr & RMR_BAE;
    }
    if (is_multicast_ether_addr(buf)) {
        if (rmr & (RMR_MAE | RMR_PMME)) {
            return true;
        }
        if (rmr & RMR_MIAE) {
            /* the top 6 bits of the CRC pick one of 64 hash table bits */
            uint32_t bit = 63 - (net_crc32(buf, ETH_ALEN) >> 26);

            return s->regs[R_GAHT1 + bit / 16] & (0x8000 >> (bit % 16));
        }
        return false;
    }
    stw_be_p(ia, s->regs[R_IAHR]);
    stl_be_p(ia + 2, s->regs[R_IALR]);
    return (rmr & RMR_IAE) && !memcmp(buf, ia, ETH_ALEN);
}

static bool emac_can_receive(NetClientState *nc)
{
    Ppc4xxEmacState *s = qemu_get_nic_opaque(nc);

    return (s->regs[R_MR0] & MR0_RXE) &&
           ppc4xx_mal_can_receive(s->mal, s->rx_chan);
}

static ssize_t emac_receive(NetClientState *nc, const uint8_t *buf,
                            size_t size)
{
    Ppc4xxEmacState *s = qemu_get_nic_opaque(nc);
    uint8_t frame[EMAC_MAX_FRAME + 4];
    size_t len;

    if (!emac_can_receive(nc)) {
        return 0;
    }
    if (!emac_rx_accept(s, buf)) {
        return size;
    }
    /* frames come without the FCS: pad short ones, add it unless stripped */
    len = MIN(size, EMAC_MAX_FRAME);
    memcpy(frame, buf, len);
    if (len < ETH_ZLEN) {
        memset(frame + len, 0, ETH_ZLEN - len);
        len = ETH_ZLEN;
    }
    if (!(s->regs[R_RMR] & RMR_SFCS)) {
        stl_le_p(frame + len, crc32(0, frame, len));
        len += 4;
    }
    trace_ppc4xx_emac_rx(s->rx_chan, len);
    if (!ppc4xx_mal_receive(s->mal, s->rx_chan, frame, len)) {
        return 0;
    }
    s->regs[R_OCRX] += len;
    return size;
}

static void emac_rx_notify(void *opaque)
{
    Ppc4xxEmacState *s = opaque;

    if (s->nic && emac_can_receive(qemu_get_queue(s->nic))) {
        qemu_flush_queued_packets(qemu_get_queue(s->nic));
    }
}

static uint64_t emac_read(void *opaque, hwaddr addr, unsigned size)
{
    Ppc4xxEmacState *s = opaque;
    uint32_t val = s->regs[addr / 4];

    if (addr / 4 == R_MR0) {
        /* the channels are idle whenever they are off */
        if (!(val & MR0_RXE)) {
            val |= MR0_RXI;
        }
        if (!(val & MR0_TXE)) {
            val |= MR0_TXI;
        }
    }
    trace_ppc4xx_emac_read(addr, val);
    return val;
}

static void emac_write(void *opaque, hwaddr addr, uint64_t val,
                       unsigned size)
{
    Ppc4xxEmacState *s = opaque;
    int reg = addr / 4;

    trace_ppc4xx_emac_write(addr, val);
    switch (reg) {
    case R_MR0:
        if (val & MR0_SRST) {
            /* the reset is done at once */
            emac_reset_regs(s);
            break;
        }
        s->regs[reg] = val & ~(MR0_RXI | MR0_TXI);
        if (val & MR0_RXE) {
            emac_rx_notify(s);
        }
        break;
    case R_TMR0:
        /* the GNP bits start the channels and read back as 0 */
        s->regs[reg] = val & ~(TMR0_GNP0 | TMR0_GNP1);
        emac_transmit(s, val);
        break;
    case R_ISR:
        s->regs[reg] &= ~val;
        emac_update_irq(s);
        break;
    case R_ISER:
        s->regs[reg] = val;
        emac_update_irq(s);
        break;
    case R_IAHR:
        s->regs[reg] = val & 0xffff;
        break;
    case R_STACR:
        emac_mdio(s, val);
        break;
    case R_OCTX:
    case R_OCRX:
        /* read only counters */
        break;
    case R_RMR:
        s->regs[reg] = val;
        emac_rx_notify(s);
        break;
    default:
        s->regs[reg] = val;
        break;
    }
}

static const MemoryRegionOps emac_ops = {
    .read = emac_read,
    .write = emac_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static NetClientInfo emac_net_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .can_receive = emac_can_receive,
    .receive = emac_receive,
};

static void emac_reset(DeviceState *dev)
{
    Ppc4xxEmacState *s = PPC4XX_EMAC(dev);

    emac_reset_regs(s);
    emac_phy_reset(s);
}

static void emac_realize(DeviceState *dev, Error **errp)
{
    Ppc4xxEmacState *s = PPC4XX_EMAC(dev);

    if (!s->mal) {
        error_setg(errp, "ppc4xx-emac needs the link to its MAL");
        return;
    }
    if (s->tx_chan + 1 >= s->mal->txcnum || s->rx_chan >= s->mal->rxcnum) {
        error_setg(errp, "ppc4xx-emac: the MAL has no such channel");
        return;
    }
    memory_region_init_io(&s->iomem, OBJECT(s), &emac_ops, s, "emac",
                          EMAC_REGS_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
    qemu_macaddr_default_if_unset(&s->conf.macaddr);
    /* an EMAC with nothing on its wire sends its frames nowhere */
    if (s->conf.peers.ncs[0]) {
        s->nic = qemu_new_nic(&emac_net_info, &s->conf,
                              object_get_typename(OBJECT(dev)), dev->id,
                              &dev->mem_reentrancy_guard, s);
        qemu_format_nic_info_str(qemu_get_queue(s->nic), s->conf.macaddr.a);
    }
    ppc4xx_mal_set_rx_notify(s->mal, s->rx_chan, emac_rx_notify, s);
}

static void emac_unrealize(DeviceState *dev)
{
    Ppc4xxEmacState *s = PPC4XX_EMAC(dev);

    if (s->nic) {
        qemu_del_nic(s->nic);
    }
}

static const VMStateDescription vmstate_emac = {
    .name = TYPE_PPC4XX_EMAC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, Ppc4xxEmacState, EMAC_REGS_SIZE / 4),
        VMSTATE_UINT16_ARRAY(phy, Ppc4xxEmacState, 32),
        VMSTATE_END_OF_LIST()
    },
};

static const Property emac_properties[] = {
    DEFINE_NIC_PROPERTIES(Ppc4xxEmacState, conf),
    DEFINE_PROP_LINK("mal", Ppc4xxEmacState, mal, TYPE_PPC4xx_MAL,
                     Ppc4xxMalState *),
    DEFINE_PROP_UINT32("tx-channel", Ppc4xxEmacState, tx_chan, 0),
    DEFINE_PROP_UINT32("rx-channel", Ppc4xxEmacState, rx_chan, 0),
    DEFINE_PROP_UINT32("phy-addr", Ppc4xxEmacState, phy_addr, 0),
};

static void emac_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = emac_realize;
    dc->unrealize = emac_unrealize;
    device_class_set_legacy_reset(dc, emac_reset);
    dc->vmsd = &vmstate_emac;
    device_class_set_props(dc, emac_properties);
    set_bit(DEVICE_CATEGORY_NETWORK, dc->categories);
    /* only works as part of a ppc4xx SoC */
    dc->user_creatable = false;
}

/* ZMII: the MII/RMII/SMII bridge between the EMACs and their PHYs */

#define ZMII_REGS   3

struct Ppc4xxZmiiState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    uint32_t regs[ZMII_REGS];   /* FER, SSR, SMIISR */
};

static uint64_t zmii_read(void *opaque, hwaddr addr, unsigned size)
{
    Ppc4xxZmiiState *s = opaque;

    return s->regs[addr / 4];
}

static void zmii_write(void *opaque, hwaddr addr, uint64_t val,
                       unsigned size)
{
    Ppc4xxZmiiState *s = opaque;

    s->regs[addr / 4] = val;
}

static const MemoryRegionOps zmii_ops = {
    .read = zmii_read,
    .write = zmii_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void zmii_reset(DeviceState *dev)
{
    Ppc4xxZmiiState *s = PPC4XX_ZMII(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static void zmii_init(Object *obj)
{
    Ppc4xxZmiiState *s = PPC4XX_ZMII(obj);

    memory_region_init_io(&s->iomem, obj, &zmii_ops, s, "zmii",
                          ZMII_REGS * 4);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static const VMStateDescription vmstate_zmii = {
    .name = TYPE_PPC4XX_ZMII,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, Ppc4xxZmiiState, ZMII_REGS),
        VMSTATE_END_OF_LIST()
    },
};

static void zmii_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    device_class_set_legacy_reset(dc, zmii_reset);
    dc->vmsd = &vmstate_zmii;
    dc->user_creatable = false;
}

static const TypeInfo ppc4xx_emac_types[] = {
    {
        .name = TYPE_PPC4XX_EMAC,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(Ppc4xxEmacState),
        .class_init = emac_class_init,
    }, {
        .name = TYPE_PPC4XX_ZMII,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(Ppc4xxZmiiState),
        .instance_init = zmii_init,
        .class_init = zmii_class_init,
    },
};

DEFINE_TYPES(ppc4xx_emac_types)
