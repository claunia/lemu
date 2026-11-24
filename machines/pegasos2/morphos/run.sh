#!/bin/sh
# Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
# SPDX-License-Identifier: GPL-2.0-or-later
# MorphOS 3.21 on a Genesi/bPlan Pegasos II (pegasos2): MPC7457 G4, 1 GB RAM,
# Radeon 7000 in the AGP slot, on-board VIA Rhine-II Ethernet and VIA AC97
# sound.
cd "$(dirname "$0")" || exit 1
case "$(uname -s)-$(uname -m)" in
Darwin-arm64) q=bin/macos-arm64/qemu-system-ppc ;;
Linux-x86_64) q=bin/linux-x86_64/qemu-system-ppc ;;
Linux-aarch64) q=bin/linux-aarch64/qemu-system-ppc ;;
*) echo "No QEMU binary for $(uname -sm)" >&2; exit 1 ;;
esac
# SDL on macOS drops the click that focuses a window
export SDL_MOUSE_FOCUS_CLICKTHROUGH=1
# There is no Pegasos II firmware: QEMU's Virtual Open Firmware (vof.bin)
# builds the device tree and starts boot.img, MorphOS's boot image, which
# mounts the disk and boots MorphOS from it. The Radeon needs no VGA BIOS.
# The CD-ROM drive is empty: change cd0 file.iso in the monitor.
# The terminal holds the QEMU monitor.
exec "$q" \
 -name "MorphOS 3.21 G4 PEGASOS2" \
 -M pegasos2 -m 1024 \
 -kernel boot.img \
 -vga none -device ati-vga,model=rv100,romfile=,bus=pci.0 \
 -drive file=morphos.qcow2,if=ide,index=0,format=qcow2 \
 -drive if=ide,index=2,media=cdrom,id=cd0 \
 -nic user,model=via-rhine \
 -monitor stdio \
 "$@"
