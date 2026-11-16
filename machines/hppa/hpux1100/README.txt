HP-UX 11.00 (B.11.00) on QEMU's HP B160L (four PA-7300LC CPUs, 512 MB, Artist graphics, SCSI disk, Tulip network)

Run ./run.sh (macOS 26+ arm64, Linux x86_64/aarch64 with glibc 2.34+ and X11/XWayland) or run.cmd (Windows 10+ x64).
Log in as root, password root, at the CDE login in the QEMU window (1280x1024, about 90 s after the start), or telnet localhost 11023.
Installed: HP-UX 11.00 (32-bit, CDE) from the Core OS CD of March 2004, with the Quality Pack QPK1100 and the Hardware Enablement bundle HWE1100 of March 2004. Not installed: the Support Tools (diagnostics).
Click in the QEMU window to grab the mouse; Ctrl-Alt-G (Ctrl-Option-G on a Mac) lets it go.
Extra QEMU options are passed through, e.g. ./run.sh -display vnc=127.0.0.1:1. With -display none the CDE login still runs, out of sight: use telnet.
The terminal holds the QEMU monitor. quit there stops QEMU at once.
Network is QEMU user mode NAT on 192.168.1.0/24. HP-UX has the fixed address 192.168.1.110 (hostname hpux11), and QEMU is its gateway and DNS server, 192.168.1.1. Host names come from /etc/hosts, then from DNS.
Run shutdown -h -y 0 as root. When HP-UX says "System has halted", type quit in the monitor.
macOS: if the binary is blocked, run xattr -cr . in this folder.

AI agents (macOS, Linux): in this folder, claude mcp add lemu -s user -- "$PWD/bin/macos-arm64/qemu-system-hppa" -mcp-bridge (on Linux bin/linux-x86_64 or bin/linux-aarch64) lets an agent such as Claude Code see the screen and use the keyboard, mouse and serial ports of the running VM. See MCP.md in the QEMU source.
QEMU source: https://github.com/tenox7/lemu branch master (GPLv2, see bin/COPYING). qemu-system-hppa --version names the commit.
