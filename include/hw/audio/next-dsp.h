/*
 * NeXT DSP56001 host interface
 *
 * Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
 *
 * This code is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef HW_AUDIO_NEXT_DSP_H
#define HW_AUDIO_NEXT_DSP_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_NEXT_DSP "next-dsp"
OBJECT_DECLARE_SIMPLE_TYPE(NeXTDSPState, NEXT_DSP)

struct NeXTDSPState {
    SysBusDevice parent_obj;

    MemoryRegion mem;
    qemu_irq irq;
};

#endif /* HW_AUDIO_NEXT_DSP_H */
