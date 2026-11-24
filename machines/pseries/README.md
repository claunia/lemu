# IBM pSeries (sPAPR)

`-M pseries` runs AIX 5.3, 6.1, 7.1, 7.2 and 7.3 as a logical partition
under TCG. Each system with a ready-to-run tarball has a folder here with
its `run.sh`, `run.cmd` and `README.txt`: `aix53`, `aix61`, `aix71` and
`aix72`. AIX 7.3 has no tarball yet.

AIX 5.3 needs device-tree fixes that `aix53/run.sh` gives SLOF as nvramrc
Forth, and it runs CDE on an ATI ES1000 with a USB keyboard and mouse.

AIX 6.1 (6100-09) runs with the same fixes and needs no others. `aix61/run.sh`
has no serial console: the AIX console is the screen, and the terminal holds
the QEMU monitor. AIX 7.1 (7100-05) runs the same way: `aix71/run.sh` differs
only in its names, ports and 4 GB of memory.

`-M pseries,panel=on` adds an operator panel window, like the 40P front panel:
a power light and a display of 2 lines of 16 characters. SLOF's checkpoint
codes and the AIX boot messages and progress codes show there instead of on
the serial console.

AIX 7.3 needs the machine options from github.com/spuhpointer/qemu branch
`aix73-papr`. Its loader takes QEMU only as `qemu,pseriesNOPE`, which
`aix-compat=on` gives (AIX then prints "qemu,pseries detected").
`aix-papr=on` makes the partition look like PowerVM instead, so ASO,
`lparstat -i` and `lssrad -av` work, with 2 NUMA nodes. Give the disk a
fixed `x-aix-vscsi-id` and use one spapr-vscsi adapter: disk at target 0,
CD at `scsi-id=1`. The AIX 7.3 boot image is over 32 MB, so raise SLOF's
PREP load limit in nvramrc:
`dev /packages/disk-label h# 80000 to max-prep-partition-blocks device-end`.
With POWER9, 4 CPUs (`cap-cfpc=broken,cap-sbbc=broken,cap-ibs=broken,cap-ccf-assist=off`)
and `ic-mode=xics`, the 7300-03-00 DVD installs in about 100 minutes with
`aix-papr=off`, as the fork did it. The first boot with `aix-papr=on`
then finds the disk as `hdisk1`; that stays, since a busy disk can't be
renamed. AIX 7.3 has no rshd until `bos.net.tcp.rcmd_server` is installed
from the DVD, rshd wants a name for the client's address (add 10.0.2.2 to
`/etc/hosts` on NAT), and root's first login forces a 10 character password.

`aix72/install.sh` installs AIX 7.2 on an emulated POWER8 from `hdisk0.qcow2`
(created on first run), falling back to the install DVD next to it;
`./install.sh cd` forces the DVD. Use the TL5 SP3 DVD
`AIX_v7.2_Install_7200-05-03-2147_DVD_1_of_2.iso`, TL5 SP10 hangs right after
the kernel banner.

## Tips for agents

- **Graphical install:** press F1 at "define the System Console" on the
  screen you want as the console.
- **Slow start:** AIX is busy for about 7 minutes after boot, and
  `oslevel -s` takes over 2 minutes. Use long timeouts.
- **Java Configuration Assistant:** on its license pages, Enter means
  Decline. Use Tab and Shift+Tab, then Space.
- **Text installs over telnet** need `TERM=vt100` and `DISPLAY` unset.
- **grep** has no `-A` or `-B`. Use `awk`.
- **Panel:** with `panel=on`, `/usr/lib/methods/showled` shows 4
  characters, and AIX changes hex letters: 0xCAFE shows "ac0e".

## Release

`release/` builds the tarballs: `build-linux.sh amd64|arm64 OUT` and
`build-windows.sh OUT` (Docker), and `build-macos.sh OUT` (Homebrew glib,
pixman, libslirp, sdl2-compat, sdl3, dylibbundler) build self-contained
qemu-system-ppc64 binaries into OUT, then
`mkrelease.sh aix53|aix61|aix71|aix72 VMDIR OUT NAME.tar.lz` packs them with
the VM's disk (`aix53.qcow2`, `aix61.qcow2`, `aix71.qcow2` or
`hdisk0.qcow2`) and the SLOF firmware.
