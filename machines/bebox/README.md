# Be Inc. BeBox (dual PowerPC 603)

`-M bebox` in `qemu-system-ppc` is the BeBox: two 603e CPUs (`-smp 1` for
one), an MPC105 host bridge, an 82378 PCI/ISA bridge, an 82091AA Super I/O
(floppy, serial 1 and 2, parallel, IDE), the 8042 keyboard controller, a
TL16C554 (serial 3 and 4, MIDI 1 and 2), a CS4231A codec, a 53C810 SCSI
controller and a Cirrus GD5434 in PCI slot 1. RAM is 8 to 256 MiB, 32 MiB
by default.

It runs Be's own boot ROM. It is not in the tree: build a flash image from
the two ROM files with `tools/mkflash.py` and give it as a pflash drive. The
guest can rewrite it, as Be's ROM updater does, so work on a copy.

    tools/mkflash.py bootnub.rom bootmain.rom flash.bin
    qemu-system-ppc -M bebox -drive if=pflash,format=raw,file=flash.bin ...

`bootnub.rom` (16 KiB) is the nub that starts both CPUs and sets up the
MPC105; `bootmain.rom` is boot ROM 1.1d7 (April 1996). Together they fit the
BeBox's 128 KiB Am29F010. BeOS R4 needs the newer boot ROM it carries as
`/beos/etc/bebox_bootmain.image` (November 1998, 315 KiB), which only fits
the 512 KiB Am29F040 that Be's flash driver also takes:

    tools/bfs.py beos-r4.iso@160186s get /beos/etc/bebox_bootmain.image main.image
    tools/mkflash.py bootnub.rom main.image flash-r4.bin

The flash chip follows the image size: 128 KiB is an Am29F010, 512 KiB an
Am29F040.

## CDs

The BeOS R4 CD and later ones have several tracks: ISO 9660 and HFS for other systems,
then BFS volumes for x86 and PowerPC. The boot ROM finds the PowerPC volume
from the table of contents. `tools/bin2iso.py` turns a MODE1/2352 `.cue` and
`.bin` dump into a 2048-byte image and prints the track starts for the
`tracks` property of `scsi-cd`:

    tools/bin2iso.py "BeOS Release 4.cue" beos-r4.iso      # tracks=0:3402:160186
    -drive if=none,id=cd0,media=cdrom,readonly=on,format=raw,file=beos-r4.iso
    -device scsi-cd,drive=cd0,scsi-id=3,tracks=0:3402:160186

The R3 PowerPC CD has one track: give it without `tracks`.

`tools/bfs.py` lists and copies files from a BFS volume, read-only.

## Debugging

The kernel debugger and the boot ROM's debug output use serial 4, the
fourth `-serial`, at 19200 baud.
