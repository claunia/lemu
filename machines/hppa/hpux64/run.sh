#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# 64-bit HP-UX 11i v1 (B.11.11) or 11i v2 (B.11.23) on an HP C3700 (PA-8700): one CPU, 2 GB RAM.
# usage: ./run.sh 11.11|11.23 [qemu args]   (disk hpux1111.qcow2 or hpux1123.qcow2 here)
cd "$(dirname "$0")" || exit 1
# No kit yet: QEMU comes from $QEMU, else from LEMU's build folder. That build finds the
# firmware (SeaBIOS-hppa 27 with the HP-UX 11 fixes) in pc-bios/hppa-firmware64.img.
q=${QEMU:-../../../build/qemu-system-hppa}
case "$1" in
11.11) name="HP-UX 11i v1" disk=hpux1111.qcow2 port=11113 ;;
11.23) name="HP-UX 11i v2" disk=hpux1123.qcow2 port=11233 ;;
*) echo "usage: $0 11.11|11.23 [qemu args]" >&2; exit 1 ;;
esac
shift
# The serial console and the QEMU monitor share this terminal: Ctrl-A c switches between
# them, Ctrl-A x quits QEMU. Halt HP-UX first with reboot -h.
# The guest has the fixed address 10.0.2.15 (QEMU's user network): telnet 127.0.0.1 $port.
# A window shows an HP VISUALIZE-EG PCI card (artist-pci): the HP-UX ITE and CDE (see
# README.md). GFX=0 ./run.sh ... runs without the card and the window.
if [ "${GFX:-1}" = 0 ]; then
	gfx="-nographic"
else
	gfx="-device artist-pci -serial mon:stdio"
fi
exec "$q" \
	-name "$name PA-RISC C3700" \
	-M C3700 -smp 1 -m 2048 \
	-accel tcg,thread=multi,tb-size=512 \
	-boot c -boot menu=off \
	-drive if=scsi,bus=0,index=6,file="$disk",format=qcow2 \
	-nic user,model=tulip,hostfwd=tcp:127.0.0.1:$port-10.0.2.15:23 \
	$gfx -audio none \
	"$@"
