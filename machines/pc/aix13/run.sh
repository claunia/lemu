#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
cd "$(dirname "$0")" || exit 1
case "$(uname -s)-$(uname -m)" in
Darwin-arm64) q=bin/macos-arm64/qemu-system-i386 ;;
Linux-x86_64) q=bin/linux-x86_64/qemu-system-i386 ;;
Linux-aarch64) q=bin/linux-aarch64/qemu-system-i386 ;;
*) echo "No QEMU binary for $(uname -sm)" >&2; exit 1 ;;
esac
# The clock runs 28 years back (same weekdays): AIX PS/2 shows 1970 for 2026.
# AIX was installed with this disk geometry: keep it. README.txt explains the
# floppy and interrupt options. The S3 86C928 card must have exactly 1 MB: only
# then does AIX's X server take it, for 1024x768 with 256 colours.
d=$(date +%Y-%m-%dT%H:%M:%S)
exec "$q" \
 -name "AIX PS/2 1.3 486 PC" \
 -M pc-i440fx-11.1,fd-bootchk=off -cpu 486 -m 16 \
 -global isa-i8259.edge-irq-withdrawal=on \
 -global isa-fdc.cmd-drops-irq=on -global isa-fdc.seek-latency-us=1000 \
 -global isa-fdc.fallback=144 \
 -rtc base=$((${d%%-*} - 28))-${d#*-} \
 -vga none -device s3-928,vram-size-mb=1,romfile=fw/928-n9.bin \
 -drive file=aix13.qcow2,if=none,id=hd0,format=qcow2 \
 -device ide-hd,drive=hd0,bus=ide.0,unit=0,cyls=1024,heads=16,secs=63,bios-chs-trans=none \
 -drive if=floppy,index=0,id=fd0 \
 -boot order=c,menu=off \
 -netdev user,id=n0,hostfwd=tcp:127.0.0.1:1323-:23,hostfwd=tcp:127.0.0.1:13514-:514 -device pcnet,netdev=n0,romfile= \
 -monitor stdio \
 "$@"
