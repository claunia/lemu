#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Zero the free space of an A/UX root file system (4.2 BSD UFS) in a disk image,
# before a kit is packed: all free blocks and free fragments, read from the
# cylinder groups' free maps. Run it on a copy, after A/UX was shut down cleanly.
# A fill from inside A/UX leaves the free fragments of partly used blocks alone,
# with the old data of deleted files in them.
# START is the partition's first 512-byte sector in the Apple partition map
# ("UNIX Root&Usr slice 0"). Without --write it only counts.
# usage: python3 -I ffszero.py IMAGE START [--write]
import struct
import sys

if len(sys.argv) not in (3, 4) or sys.argv[3:] not in ([], ["--write"]):
    sys.exit("usage: ffszero.py IMAGE START [--write]")
write = sys.argv[3:] == ["--write"]
part = int(sys.argv[2]) * 512
f = open(sys.argv[1], "r+b" if write else "rb")


def rd(off, n):
    f.seek(off)
    return f.read(n)


# struct fs, big-endian (68k), at 8 KB into the partition
sb = rd(part + 8192, 2048)
def L(o): return struct.unpack(">i", sb[o:o + 4])[0]
cblkno, cgoffset, cgmask = L(12), L(24), L(28)
size, ncg, bsize, fsize, frag, fpg = L(36), L(44), L(48), L(52), L(56), L(188)
if struct.unpack(">i", sb[1372:1376])[0] != 0x011954:
    sys.exit("no 4.2 BSD file system at that sector")
print("%d frags of %d bytes, %d cylinder groups" % (size, fsize, ncg))
zero = bytes(fsize)
free = dirty = 0
for c in range(ncg):
    base = fpg * c
    cg = rd(part + (base + cgoffset * (c & ~cgmask) + cblkno) * fsize, bsize)
    # struct cg: cg_cgx at 12, cg_cs at 24, cg_magic at 980, cg_free at 984
    cgx, nbfree, nffree = (struct.unpack(">i", cg[o:o + 4])[0] for o in (12, 28, 36))
    if cgx != c or struct.unpack(">i", cg[980:984])[0] != 0x090255:
        sys.exit("cylinder group %d: bad header" % c)
    n = min(fpg, size - base)
    bits = [i for i in range(n) if cg[984 + (i >> 3)] & (1 << (i & 7))]
    if len(bits) != nbfree * frag + nffree:
        sys.exit("cylinder group %d: free map and summary differ" % c)
    for i in bits:
        off = part + (base + i) * fsize
        if rd(off, fsize) != zero:
            dirty += 1
            if write:
                f.seek(off)
                f.write(zero)
    free += len(bits)
print("%d free frags, %d not zero%s" % (free, dirty, ", zeroed" if write else ""))
