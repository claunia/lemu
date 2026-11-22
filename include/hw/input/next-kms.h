/*
 * NeXT keyboard, mouse and sound interface (KMS)
 *
 * Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
 *
 * This code is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef HW_INPUT_NEXT_KMS_H
#define HW_INPUT_NEXT_KMS_H

#include "hw/core/sysbus.h"
#include "qom/object.h"
#include "ui/input.h"

#define TYPE_NEXT_KMS "next-kms"
OBJECT_DECLARE_SIMPLE_TYPE(NeXTKMSState, NEXT_KMS)

enum {
    NEXT_KMS_IRQ_KEYMOUSE,
    NEXT_KMS_IRQ_MONITOR,
    NEXT_KMS_IRQ_SOUND_OVRUN,
    NEXT_KMS_IRQ_NMI,
    NEXT_KMS_NUM_IRQS
};

/* A sound command for the monitor: command 0 stops all sound */
typedef void (*NeXTKMSSoundCommand)(void *opaque, uint8_t command,
                                    uint32_t data);
/* The monitor asks for more sound */
typedef void (*NeXTKMSSoundRequest)(void *opaque);
typedef void (*NeXTKMSPowerKey)(void *opaque, bool down);

struct NeXTKMSState {
    SysBusDevice parent_obj;

    MemoryRegion mem;
    qemu_irq irq[NEXT_KMS_NUM_IRQS];
    QemuInputHandlerState *hs;

    bool turbo;

    uint8_t status_snd;
    uint8_t status_km;
    uint8_t status_tx;
    uint8_t status_cmd;
    uint8_t command;
    uint32_t data;
    uint32_t kmdata;

    uint8_t rev;
    uint32_t km_addr;
    uint32_t km_data[16];
    uint32_t km_mask;

    uint8_t modifiers;
    bool button_left;
    bool button_right;
    bool buttons_changed;
    int dx, dy;

    NeXTKMSSoundCommand sound_command;
    NeXTKMSSoundRequest sound_request;
    void *sound_opaque;
    NeXTKMSPowerKey power_key;
    void *power_opaque;
};

void next_kms_set_sound(NeXTKMSState *s, NeXTKMSSoundCommand command,
                        NeXTKMSSoundRequest request, void *opaque);
void next_kms_set_power_key(NeXTKMSState *s, NeXTKMSPowerKey fn,
                            void *opaque);

/* From the sound hardware */
void next_kms_sound_request(NeXTKMSState *s);
void next_kms_sound_underrun(NeXTKMSState *s);
void next_kms_sound_in_overrun(NeXTKMSState *s);
void next_kms_codec_in(NeXTKMSState *s);

#endif /* HW_INPUT_NEXT_KMS_H */
