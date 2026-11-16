IBM AIX PS/2 1.3 (PTF 0024) as a 2-site TCF cluster: two QEMU PCs (each 486, 16 MB, i440FX, S3 86C928 with 1 MB)

TCF (Transparent Computing Facility, from Locus Computing) makes several AIX PS/2 machines one system: one file tree and one process space. AIX PS/2 1.3 has it in its kernel, turned off. Here it is on. IBM's TCF commands (clusterstart and the like) were a separate product that seems lost, so the kit has stand-ins.
Run ./run.sh (macOS 26+ arm64, Linux x86_64/aarch64 with glibc 2.34+ and X11/XWayland) or run.cmd (Windows 10+ x64). It starts both VMs, each in its own window: site 1 is aixps2 (10.0.2.15), site 2 is aixps2b (10.0.2.16). The terminal holds site 1's QEMU monitor.
Both boot to an X11 login box (xdm) and join the cluster. Log in as root, no password: type root, then press Enter at Password:. telnet localhost 2301 reaches site 1, 2302 site 2. rsh to localhost port 51401 (site 1) or 51402 (site 2) runs commands as root.
Try it in the AIX Shell window on site 1:
  sites                  lists the sites that are up: 1 aixps2 and 2 aixps2b
  on aixps2b uname -n    runs uname on site 2. No rsh, no login.
  tick &                 prints the time, the site it runs on and its pid, every 2 seconds
  migrate -aixps2b PID   moves tick to site 2. It keeps its pid and still prints here.
  migrate -aixps2 PID    moves it back. kill PID stops it.
In /bin/sh, migrate needs the dash before the site. Without it nothing moves.
X between the sites: first type set_link nat off in site 1's monitor (the terminal), because QEMU's NAT is on the same virtual network and resets TCP between the VMs. Then on aixps2b xeyes -display 10.0.2.15:0 & in site 1's AIX Shell runs xeyes on site 2 and shows its window on site 1 (/etc/X0.hosts lets both sites use both screens). Use the address: with the NAT off there is no DNS, and a host name makes X programs wait for minutes. While the NAT is off, rsh, telnet and the Internet don't work. set_link nat on brings them back, and ends the X programs that still run between the sites.
Files: / is replicated. Site 1 has the main copy, site 2 a full backup copy, and TCF copies each change over. /aixps2 and /aixps2b hold each site's own files: /dev, /tmp and /unix point there (<LOCAL>). /u is on site 1, with 110 MB free.
The TCF stand-ins are one program, /usr/local/bin/tcf: sites, on, onsite, migrate, probe, prober, clusterstart, clusterstop, xperm, newpgrp, migtest. Source, notes and how this cluster was made: https://files.42718.net/mirror/tenox/os/aix/1.3ps2/tcf/
Gotchas: processes started at boot, and so rsh commands, can't run on the other site or migrate (errno 100, ELOCALONLY): use a login shell, or newpgrp CMD. X programs don't migrate. The interrupt key is DEL, not Ctrl-C.
Otherwise each site is the aix13ps2-lemu kit: X11 at 1024x768 with 256 colours, Motif, the AIXwindows Desktop (xdt &), the MetaWare C compiler (cc), and in /usr/local/bin bash, gmake, gzip, xv, Mosaic, DOOM, gcc 1.39 and hcc, plus 44 X11R5 sample programs. Extra apps and notes: https://files.42718.net/mirror/tenox/os/aix/1.3ps2/apps/
The text console ("Console login:") is on Alt + Right Ctrl, AIX's key for the next virtual terminal. Without a Right Ctrl key (Mac keyboards), type sendkey alt-ctrl_r in site 1's monitor.
Network: each VM has an AMD PCnet-PCI II card with the leaix driver (github.com/rakslice/leaix). Both reach QEMU user mode NAT through site 1: gateway 10.0.2.2, DNS 10.0.2.3.
The clock runs 28 years back (same weekdays): AIX PS/2 shows 1970 for 2026.
Floppy: change fd0 file.img in site 1's monitor inserts a 1.44 MB floppy, /dev/rfd0 in AIX on site 1. The AIX floppy driver needs the floppy and interrupt options in run.sh (isa-fdc and isa-i8259): without them floppy reads time out.
Mouse: QEMU grabs it when you click in a window; Ctrl-Alt-G lets it go.
To stop: run shutdown -h now on site 2, then on site 1, and wait for "910-772 System halted" in each window. Then type quit in site 1's monitor and close site 2's window.
macOS: if the binary is blocked, run xattr -cr . in this folder.

AI agents (macOS, Linux): in this folder, claude mcp add lemu -s user -- "$PWD/bin/macos-arm64/qemu-system-i386" -mcp-bridge (on Linux bin/linux-x86_64 or bin/linux-aarch64) lets an agent such as Claude Code see the screens and use the keyboards, mice and serial ports of the running VMs. See MCP.md in the QEMU source.
QEMU source: https://github.com/tenox7/lemu branch master (GPLv2, see bin/COPYING). qemu-system-i386 --version names the commit.
