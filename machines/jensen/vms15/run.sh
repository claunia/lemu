#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
cd "$(dirname "$0")" || exit 1
case "$(uname -s)-$(uname -m)" in
Darwin-arm64) q=bin/macos-arm64/qemu-system-alpha ;;
Linux-x86_64) q=bin/linux-x86_64/qemu-system-alpha ;;
Linux-aarch64) q=bin/linux-aarch64/qemu-system-alpha ;;
*) echo "No QEMU binary for $(uname -sm)" >&2; exit 1 ;;
esac
f=%Y-%m-%dT%H:%M:%S
rtc=$(date -v-28y +$f 2>/dev/null || date -d '28 years ago' +$f)
# VMS keeps its address 192.168.1.15. QEMU answers as 192.168.1.1 (gateway
# and DNS), and this computer is 192.168.1.10.
n=user,id=n0,net=192.168.1.0/24,host=192.168.1.10,dns=192.168.1.1
n=$n,hostfwd=tcp:127.0.0.1:1523-192.168.1.15:23,hostfwd=tcp:127.0.0.1:1514-192.168.1.15:514
exec "$q" \
 -name "OpenVMS AXP 1.5-1H1 ALPHA JENSEN" \
 -M jensen,rtc-ram-file=rtc.bin,board-id=off -m 128 \
 -rtc base="$rtc" \
 -drive if=pflash,index=0,format=raw,file=feprom0.bin \
 -drive if=pflash,index=1,format=raw,file=feprom1.bin \
 -device jensen-aha1742,id=scsi0 \
 -drive if=none,id=hd0,file=vms15.qcow2,format=qcow2 \
 -device scsi-hd,bus=scsi0.0,scsi-id=0,drive=hd0,quirk_mode_page_rw_error_vms=on,quirk_mode_page_format_device=on \
 -drive if=none,id=cd0,media=cdrom,readonly=on \
 -device scsi-cd,bus=scsi0.0,scsi-id=4,drive=cd0,physical_block_size=512,quirk_mode_page_set_block_size=on,quirk_mode_sense_rom_use_dbd=on,quirk_mode_page_rw_error_vms=on,quirk_mode_sense_rom_wp=on,scsi_version=2,vendor=DEC,product='RRD42   (C) DEC',ver=4.5d \
 -drive if=floppy,index=0,id=fd0 \
 -netdev "$n" -device jensen-de422,netdev=n0 \
 "$@"
