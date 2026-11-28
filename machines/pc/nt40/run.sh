#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
cd "$(dirname "$0")" || exit 1
case "$(uname -s)-$(uname -m)" in
Darwin-arm64) q=bin/macos-arm64/qemu-system-i386 ;;
Linux-x86_64) q=bin/linux-x86_64/qemu-system-i386 ;;
Linux-aarch64) q=bin/linux-aarch64/qemu-system-i386 ;;
*) echo "No QEMU binary for $(uname -sm)" >&2; exit 1 ;;
esac
exec "$q" \
 -name "Windows NT 4.0 X86 PC" \
 -M pc-i440fx-11.1 -cpu pentium2 -m 256 \
 -rtc base=localtime \
 -vga cirrus \
 -drive file=nt40.qcow2,if=none,id=hd0,format=qcow2 \
 -device ide-hd,drive=hd0,bus=ide.0,unit=0 \
 -drive if=none,id=cd0,media=cdrom,readonly=on \
 -device ide-cd,drive=cd0,bus=ide.1,unit=0 \
 -drive if=floppy,index=0,id=fd0 \
 -boot order=c,menu=off \
 -netdev user,id=n0,hostfwd=tcp:127.0.0.1:43514-:514,hostfwd=tcp:127.0.0.1:43389-:3389 -device pcnet,netdev=n0 \
 -monitor stdio \
 "$@"
