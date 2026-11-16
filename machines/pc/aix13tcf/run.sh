#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Two VMs, one AIX PS/2 TCF cluster. Site 2 (aixps2b) starts in the
# background; its card is only on a link to site 1 (127.0.0.1:24600) and it
# connects again until site 1 listens. Site 1 (aixps2) runs here, with the
# QEMU monitor in this terminal; a hub joins its card, QEMU's NAT and the
# link, so both sites reach the NAT.
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
pc="-M pc-i440fx-11.1,fd-bootchk=off -cpu 486 -m 16
 -global isa-i8259.edge-irq-withdrawal=on
 -global isa-fdc.cmd-drops-irq=on -global isa-fdc.seek-latency-us=1000
 -global isa-fdc.fallback=144
 -rtc base=$((${d%%-*} - 28))-${d#*-}
 -vga none -device s3-928,vram-size-mb=1,romfile=fw/928-n9.bin
 -drive if=floppy,index=0,id=fd0
 -boot order=c,menu=off"
hd="if=none,id=hd0,format=qcow2 -device ide-hd,drive=hd0,bus=ide.0,unit=0,cyls=1024,heads=16,secs=63,bios-chs-trans=none"
"$q" -name "AIX PS/2 1.3 TCF site 2 aixps2b" $pc \
 -drive file=site2.qcow2,$hd \
 -netdev stream,id=link,server=off,addr.type=inet,addr.host=127.0.0.1,addr.port=24600,reconnect-ms=1000 \
 -device pcnet,netdev=link,romfile=,mac=52:54:00:12:34:57 \
 < /dev/null &
exec "$q" -name "AIX PS/2 1.3 TCF site 1 aixps2" $pc \
 -drive file=site1.qcow2,$hd \
 -netdev user,id=nat,hostfwd=tcp:127.0.0.1:2301-10.0.2.15:23,hostfwd=tcp:127.0.0.1:2302-10.0.2.16:23,hostfwd=tcp:127.0.0.1:51401-10.0.2.15:514,hostfwd=tcp:127.0.0.1:51402-10.0.2.16:514 \
 -netdev stream,id=link,server=on,addr.type=inet,addr.host=127.0.0.1,addr.port=24600 \
 -netdev hubport,id=hub-nat,hubid=0,netdev=nat \
 -netdev hubport,id=hub-link,hubid=0,netdev=link \
 -netdev hubport,id=hub-card,hubid=0 \
 -device pcnet,netdev=hub-card,romfile=,mac=52:54:00:12:34:56 \
 -monitor stdio \
 "$@"
