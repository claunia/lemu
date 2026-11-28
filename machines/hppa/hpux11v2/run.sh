#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# HP-UX 11i v2 (B.11.23, 64-bit) on an HP C3700 (PA-8700): one CPU, 2 GB RAM,
# an HP VISUALIZE-EG PCI graphics card (artist-pci).
cd "$(dirname "$0")" || exit 1
case "$(uname -s)-$(uname -m)" in
Darwin-arm64) q=bin/macos-arm64/qemu-system-hppa ;;
Linux-x86_64) q=bin/linux-x86_64/qemu-system-hppa ;;
Linux-aarch64) q=bin/linux-aarch64/qemu-system-hppa ;;
*) echo "No QEMU binary for $(uname -sm)" >&2; exit 1 ;;
esac
# The firmware is LEMU's SeaBIOS-hppa (bin/share/qemu/hppa-firmware64.img), and the
# card's STI ROM is bin/share/qemu/hppa-sti-artist.rom.
# tb-size=512 keeps the code buffer where it is fast on Apple Silicon.
# The terminal is the serial console. Ctrl-A c = QEMU monitor (again = back), Ctrl-A x = quit.
# HP-UX has the fixed address 10.0.2.15 (hpux11v2), gateway 10.0.2.2.
exec "$q" \
	-name "HP-UX 11i v2 PA-RISC C3700" \
	-M C3700 -smp 1 -m 2048 \
	-accel tcg,thread=multi,tb-size=512 \
	-boot c -boot menu=off \
	-drive if=scsi,bus=0,index=6,file=hpux11v2.qcow2,format=qcow2 \
	-nic user,model=tulip,hostfwd=tcp:127.0.0.1:11223-10.0.2.15:23 \
	-device artist-pci \
	-serial mon:stdio \
	"$@"
