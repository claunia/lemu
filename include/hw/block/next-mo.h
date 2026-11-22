/*
 * NeXT magneto-optical disk controller
 *
 * Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
 *
 * This code is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef HW_BLOCK_NEXT_MO_H
#define HW_BLOCK_NEXT_MO_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_NEXT_MO "next-mo"
OBJECT_DECLARE_SIMPLE_TYPE(NeXTMOState, NEXT_MO)

#define NEXT_MO_NREGS 0x17

struct NeXTMOState {
    SysBusDevice parent_obj;

    MemoryRegion mem;
    qemu_irq irq;
    uint8_t regs[NEXT_MO_NREGS];
};

#endif /* HW_BLOCK_NEXT_MO_H */
