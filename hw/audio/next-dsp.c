/*
 * NeXT DSP56001 host interface
 *
 * Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
 *
 * The registers of the DSP's host port at 0x02008000: interrupt control,
 * command vector, interrupt status, interrupt vector and four data bytes.
 * Without the DSP core they answer as the Previous emulator's dsp.c does
 * when it does not emulate the DSP, which NeXT software takes as a DSP
 * that is not there.
 *
 * This code is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2 of the License,
 * or (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "hw/audio/next-dsp.h"
#include "migration/vmstate.h"

static uint64_t next_dsp_read(void *opaque, hwaddr addr, unsigned size)
{
    switch (addr & 7) {
    case 0: /* ICR */
        return 0x7f;
    case 1: /* CVR */
    case 2: /* ISR */
    case 3: /* IVR */
        return 0xff;
    default: /* data */
        return 0x00;
    }
}

static void next_dsp_write(void *opaque, hwaddr addr, uint64_t val,
                           unsigned size)
{
}

static const MemoryRegionOps next_dsp_ops = {
    .read = next_dsp_read,
    .write = next_dsp_write,
    .impl.min_access_size = 1,
    .impl.max_access_size = 1,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_BIG_ENDIAN,
};

static void next_dsp_init(Object *obj)
{
    NeXTDSPState *s = NEXT_DSP(obj);

    memory_region_init_io(&s->mem, obj, &next_dsp_ops, s, "next.dsp",
                          0x2000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static void next_dsp_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "NeXT DSP56001 host interface";
}

static const TypeInfo next_dsp_info = {
    .name = TYPE_NEXT_DSP,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(NeXTDSPState),
    .instance_init = next_dsp_init,
    .class_init = next_dsp_class_init,
};

static void next_dsp_register_types(void)
{
    type_register_static(&next_dsp_info);
}

type_init(next_dsp_register_types)
