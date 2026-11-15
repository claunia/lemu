#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Pack an A/UX kit: the disk, the PRAM and the ROM, run.sh, run.cmd, README.txt and
# qemu-system-m68k for macOS, Linux and Windows.
# aux311: A/UX 3.1.1 on the Quadra 800. VMDIR holds the disk root.img, the PRAM
# pram-aux.img and Apple's ROM Quadra800.rom, as the user's VM folder does. It is only
# read. The pack converts the disk to qcow2, which leaves out the zeroed free space.
# BINDIR is STAGE/m68k from ../../release/stage.sh.
# OUT names the kit: the folder inside is its file name without .tar.lz.
# usage: mkrelease.sh aux311 VMDIR BINDIR OUT.tar.lz
set -e
[ $# -eq 4 ] || { echo "usage: $0 aux311 VMDIR BINDIR OUT.tar.lz" >&2; exit 1; }
here=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$here/../../.." && pwd)
os=$1 vm=$2 bin=$3
case $os in
aux311) ;;
*) echo "unknown OS: $os" >&2; exit 1 ;;
esac
for f in root.img pram-aux.img Quadra800.rom; do
    [ -f "$vm/$f" ] || { echo "$0: no $vm/$f" >&2; exit 1; }
done
plats="macos-arm64 linux-x86_64 linux-aarch64 windows-x86_64"
for p in $plats; do
    [ -d "$bin/$p" ] || { echo "$0: no $bin/$p" >&2; exit 1; }
done
case $4 in
*.tar.lz) name=$(basename "$4" .tar.lz) ;;
*) echo "OUT must end in .tar.lz" >&2; exit 1 ;;
esac
out=$(cd "$(dirname "$4")" && pwd)/$(basename "$4")
t=$(mktemp -d)
trap 'rm -rf "$t"' EXIT
r=$t/$name
mkdir -p "$r/bin/share/qemu/keymaps"

qemu-img convert -O qcow2 "$vm/root.img" "$r/$os.qcow2"
cp "$vm/pram-aux.img" "$r/pram.img"
cp "$vm/Quadra800.rom" "$r/"
cp "$here/../$os/run.sh" "$here/../$os/README.txt" "$r/"
sed 's/$/\r/' "$here/../$os/run.cmd" > "$r/run.cmd"
cp "$top/COPYING" "$top/COPYING.LIB" "$r/bin/"
find "$top/pc-bios/keymaps" -type f ! -name meson.build \
    -exec cp {} "$r/bin/share/qemu/keymaps/" \;
for p in $plats; do
    cp -a "$bin/$p" "$r/bin/"
done
chmod 644 "$r/$os.qcow2" "$r/pram.img" "$r/Quadra800.rom"

COPYFILE_DISABLE=1 tar --no-mac-metadata --no-xattrs --uid 0 --gid 0 \
    --uname root --gname root -C "$t" -cf - "$name" | plzip -9 > "$out.tmp"
lzip -t "$out.tmp"
mv "$out.tmp" "$out"
