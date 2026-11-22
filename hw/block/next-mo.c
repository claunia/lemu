/*
 * NeXT magneto-optical disk controller (Fujitsu MB600310 optical storage
 * processor)
 *
 * Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
 *
 * Register layout from the Previous emulator's mo.c and ioMemTabNEXT.c
 * (GPL-2.0-or-later). The controller is on every machine without the
 * Turbo chipset, even where no drive can be fitted, and the ROM resets it
 * when it enters its monitor. This model has the registers of the
 * controller with no drive connected.
 *
 * This code is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2 of the License,
 * or (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "hw/block/next-mo.h"
#include "migration/vmstate.h"

#define MO_NREGS        0x17

/* Interrupt status */
#define MOINT_OSP_MASK  0xFC

enum {
    MO_TRACKH = 0x00,
    MO_TRACKL = 0x01,
    MO_SECTOR = 0x02,
    MO_COUNT = 0x03,
    MO_INTSTATUS = 0x04,
    MO_INTMASK = 0x05,
    MO_CSR2 = 0x06,
    MO_CSR1 = 0x07,
    MO_CSRH = 0x08,
    MO_CSRL = 0x09,
    MO_ERRSTAT = 0x0a,
    MO_ECCCNT = 0x0b,
    MO_INIT = 0x0c,
    MO_FORMAT = 0x0d,
    MO_MARK = 0x0e,
    MO_FLAG0 = 0x10,
};

static void next_mo_update_irq(NeXTMOState *s)
{
    qemu_set_irq(s->irq, (s->regs[MO_INTSTATUS] & s->regs[MO_INTMASK]) != 0);
}

static MemTxResult next_mo_read(void *opaque, hwaddr addr, uint64_t *data,
                                unsigned size, MemTxAttrs attrs)
{
    NeXTMOState *s = opaque;
    unsigned reg = addr & 0x1f;

    if (reg >= MO_NREGS || reg == 0x0f) {
        return MEMTX_DECODE_ERROR;
    }
    switch (reg) {
    case MO_INIT:
    case MO_FORMAT:
    case MO_MARK:
        /* write only */
        *data = 0;
        break;
    default:
        *data = s->regs[reg];
        break;
    }
    return MEMTX_OK;
}

static MemTxResult next_mo_write(void *opaque, hwaddr addr, uint64_t data,
                                 unsigned size, MemTxAttrs attrs)
{
    NeXTMOState *s = opaque;
    unsigned reg = addr & 0x1f;
    uint8_t val = data;

    if (reg >= MO_NREGS || reg == 0x0f) {
        return MEMTX_DECODE_ERROR;
    }
    switch (reg) {
    case MO_INTSTATUS:
        /* the OSP bits are cleared by writing ones; no drive answers */
        s->regs[MO_INTSTATUS] &= ~(val & MOINT_OSP_MASK);
        break;
    case MO_ERRSTAT:
    case MO_ECCCNT:
        /* read only */
        break;
    default:
        s->regs[reg] = val;
        break;
    }
    next_mo_update_irq(s);
    return MEMTX_OK;
}

static const MemoryRegionOps next_mo_ops = {
    .read_with_attrs = next_mo_read,
    .write_with_attrs = next_mo_write,
    .impl.min_access_size = 1,
    .impl.max_access_size = 1,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_BIG_ENDIAN,
};

static void next_mo_reset_hold(Object *obj, ResetType type)
{
    NeXTMOState *s = NEXT_MO(obj);

    memset(s->regs, 0, sizeof(s->regs));
    next_mo_update_irq(s);
}

static void next_mo_init(Object *obj)
{
    NeXTMOState *s = NEXT_MO(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->mem, obj, &next_mo_ops, s, "next.mo", 0x2000);
    sysbus_init_mmio(sbd, &s->mem);
    sysbus_init_irq(sbd, &s->irq);
}

static const VMStateDescription next_mo_vmstate = {
    .name = "next-mo",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(regs, NeXTMOState, NEXT_MO_NREGS),
        VMSTATE_END_OF_LIST()
    },
};

static void next_mo_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->desc = "NeXT magneto-optical disk controller";
    dc->vmsd = &next_mo_vmstate;
    rc->phases.hold = next_mo_reset_hold;
}

static const TypeInfo next_mo_info = {
    .name = TYPE_NEXT_MO,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(NeXTMOState),
    .instance_init = next_mo_init,
    .class_init = next_mo_class_init,
};

static void next_mo_register_types(void)
{
    type_register_static(&next_mo_info);
}

type_init(next_mo_register_types)
