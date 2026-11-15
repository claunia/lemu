# A/UX 3.1.1 on the Quadra 800 (q800)

**TL;DR** `aux311/run.sh` boots A/UX 3.1.1 on `-M q800` from Apple's
Quadra 800 ROM. The machine is QEMU's own; LEMU adds the kit and the
release tools.

## Kit

[aux311/run.sh](aux311/run.sh) and [run.cmd](aux311/run.cmd) start the kit
`aux311q800-lemu` (see [README.txt](aux311/README.txt));
[release/mkrelease.sh](release/mkrelease.sh) packs it from the one build of
[../release](../release/README.md):

    sh release/mkrelease.sh aux311 VMDIR STAGE/m68k aux311q800-lemu.tar.lz

`VMDIR` holds the disk `root.img` (raw), the PRAM `pram-aux.img` and
`Quadra800.rom`. The kit gets them as `aux311.qcow2`, `pram.img` and
`Quadra800.rom`.

## Files

| File | Option | What it is |
|---|---|---|
| `Quadra800.rom` | `-bios` | Apple's ROM of the Quadra 800, 1 MB. Not in this source tree: the kit carries it. |
| `pram.img` | `-drive if=mtd` | The PRAM, 512 bytes. It keeps the Mac's settings, such as the start-up disk. |
| `aux311.qcow2` | `scsi-hd,scsi-id=0` | The disk, 2 GB, with an Apple partition map |

The disk holds three partitions:

| Partition | Type | Size | |
|---|---|---|---|
| MacOS | Apple_HFS | 488 MB | Mac OS 7 with A/UX Startup, which boots A/UX |
| Swap | Apple_UNIX_SVR2 | 133 MB | A/UX swap |
| UNIX Root&Usr slice 0 | Apple_UNIX_SVR2 | 1427 MB | `/`, a 4.2 BSD file system: 8 KB blocks, 1 KB fragments |

## Network

- The Ethernet is `-nic ...,model=dp83932` (SONIC). A/UX calls it `ao0`.
- A/UX wants an Apple MAC address (08:00:07:...). QEMU gives the q800
  08:00:07:12:34:56 when no `mac=` is set. On a bridged LAN, give each
  q800 VM its own address with that prefix.
- A/UX keeps its address in `/etc/NETADDRS`
  (`ao0 address broadcast netmask`), its name in `/etc/HOSTNAME`, and its
  name server in `/etc/resolv.conf`. It sets no default route at boot:
  `/usr/etc/route add default GATEWAY 1` adds one until the next boot.
- The kit's A/UX has the fixed address 192.168.1.2, so the kit gives slirp
  that network: `net=192.168.1.0/24,host=192.168.1.10,dns=192.168.1.1`.

## Shutdown

`/etc/shutdown -y -g0 -i0 now` (or Special > Shut Down in the Finder) ends
with "You may now switch off your Macintosh safely". The q800 has no soft
power-off, so QEMU keeps running: quit it then.

## Tested

The kit of 2026-11-14 on an Apple Silicon Mac:

- The Finder comes up about 35 s after QEMU starts, logged in as root.
- Idle, QEMU uses about 2% of one host core: the 68040 is halted 99% of
  the time.
- telnet (port 31123) and rsh (port 31514) answer. After
  `/usr/etc/route add default 192.168.1.1 1`, wget and curl reach the
  Internet.
- MacX starts from the Apple menu (answer OK to its memory question).
  xterm and SimCity (`/usr/SimCity`) then run on it. `/mac/bin/launch`
  from a telnet session fails with error -108: start Mac programs from
  the Finder.
- On Linux x86_64 and aarch64 (`../release/ltest-ppc.sh`), QEMU runs the
  kit until the test stops it.

## Making a kit

1. Copy the VM's disk and PRAM (`cp -c`) and boot the copy with the
   kit's slirp line, never on the LAN. Add `restrict=on`, so that no DNS
   query of the guest leaves the host while it is cleaned. telnetd then
   waits about a minute for its name lookup before the login prompt.
2. Log in with telnet. A/UX has no `/dev/zero`, no `grep -r` and no
   `find -name '*x'` that matches dot files. Clear root's `.sh_history`
   and `.bash_history`, `/etc/wtmp` and `/etc/utmp` (`> file`), core
   files, and the logs of programs that talked to the LAN. Keep only
   `localhost` and the guest's own name (`aux`) in `/etc/hosts`.
3. Shut down as above, quit QEMU, then zero the free space of `/`:
   `python3 -I release/ffszero.py root.img 1272096 --write` (the start
   sector of the root partition; the tool checks the file system first).
   A fill from inside A/UX misses the free fragments of partly used
   blocks, with the old data of deleted files in them.
4. Scan the raw image for the LAN's addresses and names, then pack.
