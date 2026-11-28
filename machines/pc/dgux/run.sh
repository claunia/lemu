#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
cd "$(dirname "$0")" || exit 1
case "$(uname -s)-$(uname -m)" in
Darwin-arm64) q=bin/macos-arm64/qemu-system-i386 ;;
Linux-x86_64) q=bin/linux-x86_64/qemu-system-i386 ;;
Linux-aarch64) q=bin/linux-aarch64/qemu-system-i386 ;;
*) echo "No QEMU binary for $(uname -sm)" >&2; exit 1 ;;
esac
# DG/UX keeps its fixed address 10.0.2.15 with the gateway 10.0.2.2, as
# QEMU's NAT gives them. telnet localhost 23023 reaches it, and port 23514
# its rshd.
# fw/NCR307-810.BIN is NCR's SDMS 3.07 SCSI BIOS with the 53C810's PCI ID,
# so the PC boots DG/UX from the SCSI disk.
# The SCSI card must be in PCI slot 0x0a and the Ethernet card in 0x0b: the
# DG/UX kernel has them as npsc(pci(),A) and dpen(pci(),B).
# DG/UX's dpen driver finds a 21143 only by the board name DE500-BA, and
# expects the BIOS to have turned on bus mastering.
# pf-without-pie: DG/UX's boot loader waits for the RTC periodic flag with
# its interrupt off.
exec "$q" \
 -name "DG/UX R4.20MU07 Pentium PC" \
 -M pc-i440fx-11.2 -cpu pentium -m 128 \
 -rtc base=utc -global mc146818rtc.pf-without-pie=on \
 -vga cirrus \
 -device lsi53c810,romfile=fw/NCR307-810.BIN,id=scsi0,addr=0x0a \
 -drive file=dgux.qcow2,if=none,id=hd0,format=qcow2 \
 -device scsi-hd,drive=hd0,bus=scsi0.0,scsi-id=0 \
 -drive file=068-600216-07__DG-UX_with_X_Intel_R4.20-R4.20MU07__1995-1999.iso,if=none,id=cd0,media=cdrom,readonly=on \
 -device scsi-cd,drive=cd0,bus=scsi0.0,scsi-id=4 \
 -netdev user,id=n0,hostfwd=tcp:127.0.0.1:23023-:23,hostfwd=tcp:127.0.0.1:23514-:514 \
 -device tulip,netdev=n0,srom-name=DE500-BA,bios-bus-master=on,addr=0x0b \
 -monitor stdio \
 "$@"
