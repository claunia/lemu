#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
cd "$(dirname "$0")" || exit 1
case "$(uname -s)-$(uname -m)" in
Darwin-arm64) q=bin/macos-arm64/qemu-system-i386 ;;
Linux-x86_64) q=bin/linux-x86_64/qemu-system-i386 ;;
Linux-aarch64) q=bin/linux-aarch64/qemu-system-i386 ;;
*) echo "No QEMU binary for $(uname -sm)" >&2; exit 1 ;;
esac
# SINIX-Z keeps its fixed address 10.0.2.15 with the gateway 10.0.2.2, as
# QEMU's NAT gives them. telnet localhost 24023 reaches it, and port 24514
# its rshd.
# fw/aha1540b320_330.bin: Adaptec's AHA-1540B BIOS 3.20 for port 330h.
# The SINIX kernel has the AHA at 330h IRQ 11 DMA 5, and the WD8003 at
# 240h IRQ 5 with its memory at D0000. Its wd driver takes only MACs with
# WD's prefix 00:00:c0 (QEMU's own is 52:54:00).
# edge-irqs=0x800: SeaBIOS makes IRQ 11 level-triggered (for PCI); the ISA
# AHA-1540B needs it edge-triggered, as on a real AT.
# cirrus-vga-gd5436: the SINIX X server knows the GD5436, not the 5446.
exec "$q" \
 -name "SINIX-Z 5.42 Pentium II PC" \
 -M pc-i440fx-11.2 -cpu pentium2 -m 128 \
 -global isa-i8259-pic.edge-irqs=0x800 \
 -rtc base=localtime \
 -vga none -device cirrus-vga-gd5436 \
 -device aha154x,id=scsi0,romfile=fw/aha1540b320_330.bin \
 -drive file=sinixz.qcow2,if=none,id=hd0,format=qcow2 \
 -device scsi-hd,drive=hd0,bus=scsi0.0,scsi-id=0 \
 -drive if=none,id=cd0,media=cdrom,readonly=on \
 -device scsi-cd,drive=cd0,bus=scsi0.0,scsi-id=4 \
 -serial msmouse \
 -netdev user,id=n0,hostfwd=tcp:127.0.0.1:24023-:23,hostfwd=tcp:127.0.0.1:24514-:514 \
 -device wd8003,netdev=n0,mac=00:00:c0:12:34:56,iobase=0x240,irq=5 \
 -monitor stdio \
 "$@"
