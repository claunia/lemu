#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# HP-UX 11.00 (B.11.00) on an HP B160L (PA-7300LC): four CPUs, one host thread each, 512 MB RAM.
cd "$(dirname "$0")" || exit 1
case "$(uname -s)-$(uname -m)" in
Darwin-arm64) q=bin/macos-arm64/qemu-system-hppa ;;
Linux-x86_64) q=bin/linux-x86_64/qemu-system-hppa ;;
Linux-aarch64) q=bin/linux-aarch64/qemu-system-hppa ;;
*) echo "No QEMU binary for $(uname -sm)" >&2; exit 1 ;;
esac
# The firmware is LEMU's SeaBIOS-hppa 27 (bin/share/qemu/hppa-firmware.img).
# tb-size=512 keeps the code buffer where it is fast on Apple Silicon.
# An idle vCPU sleeps (hpux-idle=on is the default). hpux-x11-pace (on by default)
# keeps the X server from dying at start.
# HP-UX has the fixed address 192.168.1.110 (hpux11), with gateway and DNS server
# 192.168.1.1. QEMU answers there for both, and is 192.168.1.2 itself.
# The terminal holds the QEMU monitor.
exec "$q" \
	-name "HP-UX 11.00 PA-RISC B160L" \
	-M B160L -smp 4 -m 512 \
	-accel tcg,thread=multi,tb-size=512 \
	-boot c -boot menu=off \
	-drive if=scsi,bus=0,index=6,file=hpux1100.qcow2,format=qcow2 \
	-nic user,model=tulip,net=192.168.1.0/24,host=192.168.1.2,dns=192.168.1.1,hostfwd=tcp:127.0.0.1:11023-192.168.1.110:23 \
	-monitor stdio \
	"$@"
