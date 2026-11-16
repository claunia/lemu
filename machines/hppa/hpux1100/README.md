# HP-UX 11.00 on the HP B160L: four CPUs

**TL;DR** `-M B160L -smp 4 -accel tcg,thread=multi,tb-size=512` runs HP-UX 11.00
(B.11.00) on four PA-7300LC CPUs, with CDE on the Artist console. Three QEMU changes:
the Tulip card marks received frames the way the 11.00 LAN driver wants, the X server's
timer calibration is paced, and a vCPU that has nothing to do sleeps instead of spinning.
LEMU's firmware (SeaBIOS-hppa 27) is enough.

## Run

[run.sh](run.sh) and [run.cmd](run.cmd) start the kit `hpux1100b160l-lemu` (see
[README.txt](README.txt)); [../release/mkrelease.sh](../release/mkrelease.sh) packs it.
They use LEMU's firmware and the disk `hpux1100.qcow2` on SCSI ID 6. The CDE login
comes up about 90 s after the start; root's password is root.

HP-UX has the fixed address 192.168.1.110 (`hpux11` in `/etc/hosts`), with gateway and
DNS server 192.168.1.1 (`/etc/rc.config.d/netconf`, `/etc/resolv.conf`). So the kit's user
network is 192.168.1.0/24: QEMU answers at 192.168.1.1 as gateway and DNS server
(`dns=`), and is 192.168.1.2 itself (`host=`). Port 11023 on the host's 127.0.0.1 goes
to telnet.

Check the guest with `ioscan -fnC processor` (four processors, all CLAIMED). With
`-trace hppa_hpux_match`, QEMU says that it found the idle loop and, as they run, the
four queue stores.

## The disk

- Ignite-UX from the Core OS CD of March 2004: 32-bit HP-UX with CDE, English
  (HPUXEng32RT B.11.00.01), unlimited users. From the Support Plus CD of March 2004: the
  Quality Pack QPK1100 (B.11.00.64.4) and the Hardware Enablement bundle HWE1100
  (B.11.00.0403.3).
- The X server on that Core OS CD is damaged (`/usr/bin/X11/X` fails its checksum, and
  Ignite skips it as `Corrupt Xserver.X11-SERV`). The one on the disk comes from the Core
  OS CD of June 2002, with the patches PHSS_20141 and PHSS_26620.
- LVM: `/stand` (HFS, 300 MB), 1 GB of swap and one VxFS `/` of 6.7 GB. There is no
  separate `/usr`, `/var`, `/opt`, `/home` or `/tmp`.
- Not installed: the Support Tools (diagnostics). They have nothing to look at on
  emulated hardware.
- Time zone PST8PDT (`/etc/TIMEZONE`).

## What changed in QEMU

- **Network.** Like a real 21143, the Tulip card now marks a received frame with RDES0 FT
  when it is an Ethernet II frame, and with RDES0 MF when it goes to a group address. The
  11.00 LAN driver (btlan) counted every frame without FT as an unknown protocol, so ARP
  and IP got nothing. See `hw/net/tulip.c`.
- **X server.** The X servers of 11.00 calibrate the interval timer with a loop, and die
  with SIGFPE when the emulated CPU counts to 200000 in less than a millisecond. dtlogin
  then says "The X-server can not be started on display hpux11:0". As for 10.20 (see
  [../hpux1020/README.md](../hpux1020/README.md)), the translator finds that loop in the
  servers of PHSS_20141, PHSS_26608 and PHSS_26620 and paces it (`hpux-x11-pace`, on by
  default).
- **Idle loop.** HP-UX never halts a CPU: its idle loop (`idle()`) polls, so every idle
  vCPU kept a host core at 100%. As for 10.20 and 11.11 (see
  [../hpux11v1/README.md](../hpux11v1/README.md)), the translator finds that loop and the
  four stores with which the scheduler (`_setrq()`) queues a thread, by their
  instructions. A vCPU halts when its run queue, the real-time queue and its requests are
  empty, and, if it is the one idle CPU that looks at the others' queues, no other CPU
  has a thread to give away. A store to a queue wakes the vCPU that waits for that queue,
  or the one that looks at the others; a store to the real-time queue wakes them all.
  An interrupt wakes a vCPU too. It knows one kernel: the one on this kit's disk (B.11.00
  with QPK1100). Another kernel, or this one linked again with other drivers, keeps
  spinning. `-M B160L,hpux-idle=off` turns it off. See `target/hppa/hpux_idle.c`.

## Speed

Apple M1 Ultra. A shell loop that runs `/usr/bin/true` and `expr` 400 times each, alone
and four loops at once, and the host CPU that QEMU uses while the guest waits at the CDE
login.

| Setup                           | 1 loop    | 4 loops | idle            |
|---------------------------------|-----------|---------|-----------------|
| 1 CPU, no idle support (before) | 4.4 s     |         | 100% of a core  |
| 1 CPU                           | 4.2-4.5 s |         | 10%             |
| 4 CPUs, `hpux-idle=off`         | 5.2-5.9 s | 9.2 s   |                 |
| **4 CPUs (the kit)**            | 5.0 s     | 8.4 s   | 5-10%           |

- What is left at idle is mostly the clock interrupts and QEMU's display.

## Known guest messages

- At shutdown, `Stopping audio server daemon ... FAIL`: the B160L model has no audio.
  It does no harm.
