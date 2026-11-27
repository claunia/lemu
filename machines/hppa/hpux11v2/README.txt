HP-UX 11i v2 (B.11.23, 64-bit) on QEMU's HP C3700 (one PA-8700 CPU, 2 GB, HP VISUALIZE-EG PCI graphics, SCSI disk, Tulip network)

Run ./run.sh (macOS 26+ arm64, Linux x86_64/aarch64 with glibc 2.34+ and X11/XWayland) or run.cmd (Windows 10+ x64).
Log in as root, no password, at the CDE login in the QEMU window (1280x1024, about 2 min after the start), on the console in the terminal, or telnet localhost 11223.
The terminal is the serial console: firmware text, HP-UX boot messages and a console login. Ctrl-A c switches to the QEMU monitor and back. Ctrl-A x stops QEMU at once.
Installed: HP-UX 11i v2 Foundation Operating Environment (FOE) of May 2005, with CDE. GNU tools: gcc/g++/gfortran 4.0.2, gdb 5.5 (HP WDB) and bzip2 in /opt/TWWfsw/bin; bash 4.3, gmake 3.80, gtar 1.20, less, GNU find, coreutils 9.4, clzip, ncftp and nedit in /usr/local/bin. Both folders are at the end of the PATH, so the HP-UX ls, find and others come first.
Click in the QEMU window to grab the mouse; Ctrl-Alt-G (Ctrl-Option-G on a Mac) lets it go.
Extra QEMU options are passed through, e.g. ./run.sh -display vnc=127.0.0.1:1. With -display none the CDE login still runs, out of sight: use the console or telnet.
Network is QEMU user mode NAT. HP-UX has the fixed address 10.0.2.15 (hostname hpux11v2) and the gateway 10.0.2.2 (the host). There is no DNS: use IP addresses, or put nameserver 10.0.2.3 in /etc/resolv.conf.
To halt, run reboot -h as root. When HP-UX says "System has halted", press Ctrl-A x.
macOS: if the binary is blocked, run xattr -cr . in this folder.

AI agents (macOS, Linux): in this folder, claude mcp add lemu -s user -- "$PWD/bin/macos-arm64/qemu-system-hppa" -mcp-bridge (on Linux bin/linux-x86_64 or bin/linux-aarch64) lets an agent such as Claude Code see the screen and use the keyboard, mouse and serial ports of the running VM. See MCP.md in the QEMU source.
QEMU source: https://github.com/tenox7/lemu branch master (GPLv2, see bin/COPYING). qemu-system-hppa --version names the commit. Notes on 64-bit HP-UX in QEMU: machines/hppa/hpux64/README.md there.
