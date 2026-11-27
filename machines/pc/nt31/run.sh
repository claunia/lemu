#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
cd "$(dirname "$0")" || exit 1
case "$(uname -s)-$(uname -m)" in
Darwin-arm64) q=bin/macos-arm64/qemu-system-i386 ;;
Linux-x86_64) q=bin/linux-x86_64/qemu-system-i386 ;;
Linux-aarch64) q=bin/linux-aarch64/qemu-system-i386 ;;
*) echo "No QEMU binary for $(uname -sm)" >&2; exit 1 ;;
esac
# fw/bios-nt31.bin: LEMU's pc-bios/bios-256k.bin with one value changed. Without it
# NTLDR sees only 64 MB. More than 1 GB hangs NTLDR.
#   cp pc-bios/bios-256k.bin bios-nt31.bin
#   printf '\074\0\0' | dd of=bios-nt31.bin bs=1 seek=245559 conv=notrunc
# fw/928-n9.bin: the card's VGA BIOS; NT's S3 driver sets every mode through it.
# 86Box's Metheus BIOS (roms/video/s3/928.VBI) with the "Number Nine Computer"
# name that NT's S3.SYS looks for, and the checksum fixed:
#   cp 928.VBI 928-n9.bin
#   printf 'Number Nine Computer' | dd of=928-n9.bin bs=1 seek=32738 conv=notrunc
#   printf '\246' | dd of=928-n9.bin bs=1 seek=32767 conv=notrunc
# NT 3.1's ATDISK.SYS reads the BIOS head count as a signed byte: keep this disk
# geometry. NT 3.1 can't use QEMU's IDE CD-ROM or SCSI cards, so there is no CD.
exec "$q" \
 -name "Windows NT 3.1 X86 PC" \
 -M pc-i440fx-11.1 -cpu pentium -m 1024 \
 -bios fw/bios-nt31.bin \
 -rtc base=localtime \
 -vga none -device s3-928,vram-size-mb=4,romfile=fw/928-n9.bin \
 -drive file=nt31.qcow2,if=none,id=hd0,format=qcow2 \
 -device ide-hd,drive=hd0,bus=ide.0,unit=0,cyls=8128,heads=16,secs=63,lcyls=1024,lheads=127,lsecs=63 \
 -drive if=floppy,index=0,id=fd0 \
 -netdev user,id=n0,hostfwd=tcp:127.0.0.1:34514-:514,hostfwd=tcp:127.0.0.1:34389-:3389 -device ne2k_isa,iobase=0x300,irq=5,netdev=n0 \
 -monitor stdio \
 "$@"
