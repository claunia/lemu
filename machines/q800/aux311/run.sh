#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# A/UX 3.1.1 on a Quadra 800 (q800): 68040, 128 MB RAM, 1152x870 screen with 256 colours.
cd "$(dirname "$0")" || exit 1
case "$(uname -s)-$(uname -m)" in
Darwin-arm64) q=bin/macos-arm64/qemu-system-m68k ;;
Linux-x86_64) q=bin/linux-x86_64/qemu-system-m68k ;;
Linux-aarch64) q=bin/linux-aarch64/qemu-system-m68k ;;
*) echo "No QEMU binary for $(uname -sm)" >&2; exit 1 ;;
esac
# The Quadra 800 starts from Apple's ROM, Quadra800.rom. pram.img holds its PRAM.
# The disk is at SCSI ID 0. Notes: machines/q800/README.md in the QEMU source.
# Network: QEMU user networking (NAT). A/UX has the fixed address 192.168.1.2, so QEMU
# is given that network: telnet port 31123 and rsh port 31514 on this host.
# The terminal holds the QEMU monitor.
exec "$q" \
	-name "A/UX 3.1.1 68040 Quadra 800" \
	-M q800 -m 128 -bios Quadra800.rom \
	-g 1152x870x8 \
	-drive file=pram.img,format=raw,if=mtd \
	-drive file=aux311.qcow2,if=none,id=hd0,format=qcow2 \
	-device scsi-hd,scsi-id=0,drive=hd0 \
	-nic user,model=dp83932,net=192.168.1.0/24,host=192.168.1.10,dns=192.168.1.1,hostfwd=tcp:127.0.0.1:31123-192.168.1.2:23,hostfwd=tcp:127.0.0.1:31514-192.168.1.2:514 \
	-monitor stdio \
	"$@"
