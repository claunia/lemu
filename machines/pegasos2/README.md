# MorphOS 3.21 on the Genesi/bPlan Pegasos II (pegasos2)

<!-- Copyright (c) 2026 Natalia Portillo <claunia@claunia.com> -->

**TL;DR** `morphos/run.sh` boots MorphOS 3.21 on `-M pegasos2` through
QEMU's Virtual Open Firmware. Graphics (Radeon 7000 in the AGP slot, 2D, 3D
and command processor), network (on-board VIA Rhine-II) and sound (VIA
AC97) work.

## Kit

[morphos/run.sh](morphos/run.sh) and [run.cmd](morphos/run.cmd) start the
kit `morphos321pegasos2-lemu` (see [README.txt](morphos/README.txt));
[release/mkrelease.sh](release/mkrelease.sh) packs it from the one build of
[../release](../release/README.md):

    sh release/mkrelease.sh morphos VMDIR STAGE/ppc morphos321pegasos2-lemu.tar.lz

`VMDIR` holds the disk `morphos.qcow2` and `boot.img`, the boot image from
the root of the MorphOS 3.21 CD.

## Files

| File | Option | What it is |
|---|---|---|
| `boot.img` | `-kernel` | MorphOS's boot image (ELF), from the CD; VOF starts it |
| `morphos.qcow2` | `if=ide`, primary master | The disk, 8 GB: DH0 `System` (1 GB) and DH1 `Work`, SFS\0, made by the MorphOS installer |
| `bin/share/qemu/vof.bin`, `dtb/pegasos2.dtb` | (none) | QEMU's Virtual Open Firmware and the device tree it starts from, from `pc-bios/` |

## Machine

| Part | Device | MorphOS driver |
|---|---|---|
| CPU | MPC7457 (G4) | |
| North bridge | Marvell MV64361 | |
| South bridge | VIA VT8231: IDE, USB, PS/2, serial | `ide.device` |
| Graphics | `ati-vga,model=rv100` (Radeon 7000) in the AGP slot, PCI unit 0 of the MV64361 | Radeon, through the CP's PIO path |
| Ethernet | `via-rhine` (VT6102) at 0d, on board | `via_rhinepci.device` (`eth1`) |
| Sound | `via-ac97` of the VT8231 | `pegasos.audio` (AHI) |

The MV64361 has two PCI units. NetBSD (`sys/arch/ofppc/pci/gt_mainbus.c`)
calls unit 0 the AGP bus: it holds the AGP slot, at 66 MHz
(`/pci@c0000000`). Unit 1 is the PCI bus with the VT8231, the Rhine and
the PCI slots (`/pci@80000000`). The chip has no AGP port or GART, so the
AGP card runs as a PCI one there.

## MorphOS notes

- "Radeon: Failed to allocate CCE DMA memory" is what a real Pegasos II
  prints too. MorphOS's Radeon driver turns on DMA only for a card with a
  PCI Express capability (its own PCI GART) or behind an Apple UniNorth or
  U3 AGP bridge (106b:002d, 0034, 0066, 004b, 0059). Otherwise it feeds the
  command processor through the PIO aperture.
- Network prefs: Use and Save only write what changed from the shown
  settings. On the CD, the DHCP entry for `eth1` is shown but not set up
  until something about it is changed. NetStack only gets the interfaces
  that the prefs write; `ENV:sys/Interfaces` by hand and `NetConfig` do not
  add one to a running NetStack.
- AHI's speaker picture in Prefs/Audio is not a test button; `C:Play` plays
  a sample.
- Install: First Flight, Installation, New Installation, automatic
  partitioning. It takes about 4 minutes.
- Rebooting: MorphOS reboots by jumping to the firmware's reset vector at
  0xfff00100. With VOF the board puts code there that resets the machine
  through the VT8231's keyboard controller. `system_reset` works too.
