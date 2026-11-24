# AmigaOS 4.1 on the ACube Sam440ep (sam440ep)

<!-- Copyright (c) 2026 Natalia Portillo <claunia@claunia.com> -->

**TL;DR** `aos41/run.sh` boots AmigaOS 4.1 on `-M sam440ep` from a U-Boot
built from source. Graphics (Radeon, 2D and CP), network (3Com 3C905C)
and sound (CS4281, playback) work.

## Kit

[aos41/run.sh](aos41/run.sh) and [run.cmd](aos41/run.cmd) start the kit
`aos41sam440ep-lemu` (see [README.txt](aos41/README.txt));
[release/mkrelease.sh](release/mkrelease.sh) packs it from the one build of
[../release](../release/README.md):

    sh release/mkrelease.sh aos41 VMDIR STAGE/ppc aos41sam440ep-lemu.tar.lz

`VMDIR` holds the disk `aos41.qcow2`. The firmware comes from this tree.

## Files

| File | Option | What it is |
|---|---|---|
| `fw/u-boot-sam440ep.bin` | `-bios` | U-Boot for the Sam440ep, 512 KB, from `pc-bios/` (see [firmware](#firmware)) |
| `fw/vgabios-ati.bin` | `ati-vga.romfile=` | QEMU's SeaVGABIOS for the Radeon, which U-Boot runs in its x86 emulator |
| `aos41.qcow2` | `if=ide`, port 0 of the SiI3114 | The disk, 8 GB, with an RDB and one FFS partition (`DOS\07`, 512 byte blocks), DH0 `System` |

## Machine

| Part | Device | AmigaOS driver |
|---|---|---|
| CPU | PowerPC 440EP | |
| SATA | SiI3114 behind the PCI-PCI bridge | `sii3114ide.device` |
| USB | `isp1561-ohci`, with a USB keyboard and mouse | `ohci.usbhcd` |
| Graphics | `ati-vga,model=rv100` at 0c, 2D and CP (the real board has a Radeon Mobility M9) | `ATIRadeon.chip` |
| Ethernet | `3c905c` in PCI slot 07 (the two on-chip EMACs are not modelled) | `eth3com.device` |
| Sound | `cs4281` at 0e, with an ALC655 codec | `cs4281.audio` (AHI) |

The NIC must not go in slot 06: U-Boot gives that slot IRQ 50 (UIC1 input
18), and the AmigaOS 4.1 kernel never enables any UIC1 input. Slot 07 is
IRQ 29 on UIC0. A `-nic` or `-device` with no slot still lands in 07.

## Firmware

`pc-bios/u-boot-sam440ep.bin` is ACube's GPL U-Boot for the Sam440ep
(`u-boot-2015.d`) with [firmware/u-boot-sam440ep.patch](firmware/u-boot-sam440ep.patch).
[firmware/build.sh](firmware/build.sh) downloads the tarball, checks it and
rebuilds the binary in the AmigaOne's Docker image:

    sh firmware/build.sh

The patch runs the Radeon's own VGA BIOS instead of ATI's M9 BIOS, fixes
the x86 emulator's PCI config data port and stack, sets VESA mode 0x111,
accepts a read delay calibration where every setting works, counts down
the boot menu (`menuboot_delay=5`) and stops USB before the kickstart is
loaded, unless it comes from USB.

## AmigaOS notes

- Install: in Media Toolbox, make DH0 FFS (`DOS\07`) with 512 byte
  blocks. SFS\00 did not format here. The install then takes over an
  hour: FFS flushes the disk after almost every write (about 55,000
  flushes, each a host fsync).
- The post-install procedure runs again at every boot until it is
  completed.
- The Internet wizard: its radio buttons did not take clicks here; the
  keyboard shortcuts work (`t` Ethernet, `m` Manual).
- The network interface is `DEVS:NetInterfaces/ETH3COM`:
  `device=eth3com.device`, `configure=dhcp`.
- Sound: choose the CS4281 HiFi 16 bit stereo mode for unit 0 in
  Prefs/AHI.
- The Radeon driver writes its CP packets to the CSQ PIO aperture in the
  chip's byte order, so `ati-vga` does not byte-swap that aperture even
  when the register aperture is big-endian.
