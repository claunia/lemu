#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Pack a PC kit: the disks and firmware from VMDIR, run.sh, run.cmd and
# README.txt from ../GUEST, the binaries from BINDIR
# usage: pack.sh nt31|nt351|nt40|aix13|aix13tcf|dellunix|dgux|sinixz VMDIR BINDIR OUT.tar.lz
#   VMDIR   holds disk/DISK.qcow2 for each disk of the guest (GUEST.qcow2;
#           aix13tcf: site1.qcow2 and site2.qcow2) and fw/928-n9.bin, for
#           nt351 also fw/bios-nt351.bin (dellunix: fw/s3t64vp.bin only, the
#           S3 Trio64V+ video BIOS; nt31: fw/bios-nt31.bin too; nt40: no fw/;
#           dgux: fw/NCR307-810.BIN only, and the DG/UX install CD's .iso;
#           sinixz: fw/aha1540b320_330.bin only); it is only read
#   BINDIR  STAGE/i386 of ../../release/stage.sh
# nt31 makes nt31x86-lemu, nt351 nt351x86-lemu, nt40 nt40x86-lemu, aix13 makes aix13ps2-lemu (AIX PS/2 1.3),
# aix13tcf makes aix13ps2-tcf-lemu (two AIX PS/2 1.3 VMs as a TCF cluster),
# dellunix makes dellunix22-lemu (Dell UNIX SVR4 Issue 2.2),
# dgux makes dgux420x86-lemu (DG/UX R4.20MU07 for Intel),
# sinixz makes sinixz542-lemu (SINIX-Z 5.42 B20).
set -e
[ $# -eq 4 ] || { echo "usage: $0 nt31|nt351|nt40|aix13|aix13tcf|dellunix|dgux|sinixz VMDIR BINDIR OUT.tar.lz" >&2; exit 1; }
here=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$here/../../.." && pwd)
g=$1 vm=$2 bin=$3 files=
# disks: VMDIR/disk/DISK.qcow2 each; fw: files from VMDIR/fw;
# roms: files from pc-bios that QEMU looks up by name;
# files: from VMDIR into the kit's folder
case $g in
nt31) k=nt31x86-lemu disks=nt31 fw="bios-nt31.bin 928-n9.bin" roms= ;;
nt351) k=nt351x86-lemu disks=nt351 fw="bios-nt351.bin 928-n9.bin" roms= ;;
nt40) k=nt40x86-lemu disks=nt40 fw= roms="bios-256k.bin vgabios-cirrus.bin efi-pcnet.rom" ;;
aix13) k=aix13ps2-lemu disks=aix13 fw=928-n9.bin roms=bios-256k.bin ;;
aix13tcf) k=aix13ps2-tcf-lemu disks="site1 site2" fw=928-n9.bin roms=bios-256k.bin ;;
dellunix) k=dellunix22-lemu disks=dellunix fw=s3t64vp.bin roms=bios-256k.bin ;;
dgux) k=dgux420x86-lemu disks=dgux fw=NCR307-810.BIN roms="bios-256k.bin vgabios-cirrus.bin"
    files=068-600216-07__DG-UX_with_X_Intel_R4.20-R4.20MU07__1995-1999.iso ;;
sinixz) k=sinixz542-lemu disks=sinixz fw=aha1540b320_330.bin roms="bios-256k.bin vgabios-cirrus.bin" ;;
*) echo "$0: no guest $g" >&2; exit 1 ;;
esac
[ -f "$here/../$g/run.sh" ] || { echo "$0: no guest $g in $here/.." >&2; exit 1; }
for f in $(for x in $disks; do echo disk/$x.qcow2; done) \
         $(for x in $fw; do echo fw/$x; done) $files; do
    [ -f "$vm/$f" ] || { echo "$0: no $vm/$f" >&2; exit 1; }
done
plats="macos-arm64 linux-x86_64 linux-aarch64 windows-x86_64"
for p in $plats; do
    [ -d "$bin/$p" ] || { echo "$0: no $bin/$p" >&2; exit 1; }
done
out=$(cd "$(dirname "$4")" && pwd)/$(basename "$4")
# the macOS binaries must be signed with the Developer ID and notarized
sh "$here/../../release/sign-mac.sh" -c "$bin/macos-arm64"
t=$(mktemp -d)
trap 'rm -rf "$t"' EXIT
r=$t/$k
mkdir -p "$r/bin/share/qemu/keymaps"

for x in $disks; do
    qemu-img convert -O qcow2 "$vm/disk/$x.qcow2" "$r/$x.qcow2"
    chmod 644 "$r/$x.qcow2"
done
if [ -n "$fw" ]; then
    mkdir -p "$r/fw"
    for x in $fw; do cp "$vm/fw/$x" "$r/fw/"; done
    chmod 644 "$r"/fw/*
fi
for x in $files; do cp "$vm/$x" "$r/"; chmod 644 "$r/$x"; done
cp "$here/../$g/run.sh" "$here/../$g/README.txt" "$r/"
sed 's/$/\r/' "$here/../$g/run.cmd" > "$r/run.cmd"
cp "$top/COPYING" "$top/COPYING.LIB" "$r/bin/"
find "$top/pc-bios/keymaps" -type f ! -name meson.build \
    -exec cp {} "$r/bin/share/qemu/keymaps/" \;
for p in $plats; do
    cp -a "$bin/$p" "$r/bin/"
done
# the PC loads kvmvapic.bin, the option ROM for the CPU's local APIC
for x in kvmvapic.bin $roms; do
    cp "$top/pc-bios/$x" "$r/bin/share/qemu/"
    cp "$top/pc-bios/$x" "$r/bin/windows-x86_64/share/"
done

COPYFILE_DISABLE=1 tar --no-mac-metadata --no-xattrs --uid 0 --gid 0 \
    --uname root --gname root -C "$t" -cf - $k | plzip -9 > "$out.tmp"
lzip -t "$out.tmp"
mv "$out.tmp" "$out"
