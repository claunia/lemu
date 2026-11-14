IBM AIX PS/2 1.3 (PTF 0024) for x86 on QEMU's PC (486, 16 MB, i440FX, VGA)

Run ./run.sh (macOS 26+ arm64, Linux x86_64/aarch64 with glibc 2.34+ and X11/XWayland) or run.cmd (Windows 10+ x64).
Log in as root (no password) at "Console login:" on the screen, or with telnet localhost 1323.
rsh to localhost port 13514 runs commands as root (no .rhosts needed).
X11: type xinit for X with twm, or xinit -mwm for the Motif window manager. In the AIX Shell window, xdt & starts the AIXwindows Desktop. Ctrl-Alt-Backspace leaves X.
Installed: the Base Operating System and 28 program products, among them TCP/IP, NFS, X Windows, AIXwindows (Motif) and its Desktop, Xstation Manager, the MetaWare High C compiler (cc), Advanced Development Tools, SCCS, INed, Message Handler, Text Formatting System, games and the man pages. All are updated to PTF 0024 (1995).
/u is empty, with 110 MB free for your files.
Network: an AMD PCnet-PCI II card with the leaix driver (github.com/rakslice/leaix, an AMD PCnet driver for AIX PS/2 ported from NetBSD; its source is in /u/src/leaix). QEMU user mode NAT; AIX is 10.0.2.15 with gateway 10.0.2.2 and DNS 10.0.2.3.
The clock runs 28 years back (same weekdays): AIX PS/2 shows 1970 for 2026.
The terminal holds the QEMU monitor: change fd0 file.img inserts a 1.44 MB floppy, /dev/rfd0 in AIX (tar, dosread, dosdir).
The AIX floppy driver needs the floppy and interrupt options in run.sh (isa-fdc and isa-i8259): without them floppy reads time out. isa-fdc.fallback=144 makes the empty drive a 1.44 MB drive. Otherwise QEMU makes it 2.88 MB, a size AIX doesn't know, and AIX then drops the floppy device at boot.
Mouse: QEMU grabs it when you click in the window; Ctrl-Alt-G lets it go.
QEMU halts the CPU while AIX is idle, so an idle VM uses about 2% of a host core.
Run shutdown -h now and wait for "910-772 System halted" before quitting QEMU.
macOS: if the binary is blocked, run xattr -cr . in this folder.

AI agents (macOS, Linux): in this folder, claude mcp add lemu -s user -- "$PWD/bin/macos-arm64/qemu-system-i386" -mcp-bridge (on Linux bin/linux-x86_64 or bin/linux-aarch64) lets an agent such as Claude Code see the screen and use the keyboard, mouse and serial ports of the running VM. See MCP.md in the QEMU source.
QEMU source: https://github.com/tenox7/lemu branch master (GPLv2, see bin/COPYING). qemu-system-i386 --version names the commit.
