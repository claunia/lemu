#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
cd "$(dirname "$0")" || exit 1
case "$(uname -s)-$(uname -m)" in
Darwin-arm64) q=bin/macos-arm64/qemu-system-i386 ;;
Linux-x86_64) q=bin/linux-x86_64/qemu-system-i386 ;;
Linux-aarch64) q=bin/linux-aarch64/qemu-system-i386 ;;
*) echo "No QEMU binary for $(uname -sm)" >&2; exit 1 ;;
esac
# Dell UNIX keeps its fixed address 192.168.1.111: QEMU's NAT network is
# 192.168.1.0/24, with the gateway and DNS at 192.168.1.1.
# Dell UNIX was installed with this disk geometry: keep it. Its WD8003E
# driver is set for I/O 0x280, IRQ 5 and memory 0xD0000.
# fw/s3t64vp.bin is S3's video BIOS for the Trio64V+. An empty floppy drive
# is a 1.44 MB one: QEMU's default, 2.88 MB, is unknown to Dell UNIX.
exec "$q" \
 -name "Dell UNIX SVR4 2.2 Pentium PC" \
 -M pc-i440fx-11.1 -cpu pentium -m 64 \
 -rtc base=localtime \
 -vga none -device s3-trio64,vgamem_mb=4,romfile=fw/s3t64vp.bin \
 -drive file=dellunix.qcow2,if=none,id=hd0,format=qcow2 \
 -device ide-hd,drive=hd0,bus=ide.0,unit=0,cyls=1015,heads=16,secs=63,bios-chs-trans=none \
 -drive if=floppy,index=0,id=fd0 -global isa-fdc.fallback=144 \
 -boot order=c,menu=off \
 -chardev msmouse,id=ms -serial chardev:ms -serial null \
 -netdev user,id=n0,net=192.168.1.0/24,host=192.168.1.10,dns=192.168.1.1,hostfwd=tcp:127.0.0.1:22023-192.168.1.111:23 \
 -device wd8003,netdev=n0,iobase=0x280,irq=5 \
 -monitor stdio \
 "$@"
