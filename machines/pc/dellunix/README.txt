Dell UNIX System V Release 4.0 Issue 2.2 (1992) for x86 on QEMU's PC (Pentium, 64 MB, i440FX, Cirrus CL-GD5446 with 4 MB, WD8003E Ethernet)

Run ./run.sh (macOS 26+ arm64, Linux x86_64/aarch64 with glibc 2.34+ and X11/XWayland) or run.cmd (Windows 10+ x64).
Dell UNIX boots to an X11 login box (xdm) in about 10 seconds. Log in as root, no password: type root, then press Enter at Password:. telnet localhost 22023 works too.
X11: after the login you get the Motif window manager (mwm), an xterm and a clock; click a window to type in it. Hold the right mouse button on the background for Dell's menu: New Window (an xterm), Applications, Shutdown (Powerdown or Reboot) and Quit/Logout. Ctrl-Alt-Backspace restarts X.
X runs 1280x1024 with 256 colours on the Cirrus card. For 65536 colours add -bpp 16, for 1600x1200 add -xf86config /etc/XF86Config.1600, at the end of the :0 line in /usr/X5/lib/X11/xdm/Xservers, then reboot. To boot to the text console instead: rm /etc/rc2.d/S99xdm and reboot; startx then starts X.
The X server is XFree86 3.3.6 for UnixWare. It runs on Dell UNIX with a small library and a 71-byte patch; /usr/X11R6/README.dell says what they do. Dell's own X11R5 server, Xmach, is still in /usr/X5/bin.
Installed: Dell UNIX with X11R5 (mwm, olwm, twm, xterm, xman, xv, xfig, tgif and many more in /usr/X5/bin), GNU emacs, gcc 2.1, gdb, make, perl 4, TeX and LaTeX with xdvi, tcsh 6.01, less, top, elm, rn and nn, kermit and the manual pages.
Extra apps in /usr/local/bin (already in the PATH): bash 1.14.7, gzip, wget 1.5.3, ncftp, NCSA Mosaic 2.5 (mosaic) and 1.2 (xmosaic), and the JPEG tools.
Web: mosaic & in the xterm. Mosaic 2.5 (1994) speaks plain HTTP only, sends no Host: header and saves pages that name a charset as files, so few sites work today. http://textfiles.com/ does, but Mosaic crashes on about half of its loads ("Congratulations, you have found a bug"): start it again.
Network: a Western Digital WD8003E ISA card, set as Dell's driver wants it (I/O 0x280, IRQ 5, memory 0xD0000). QEMU user mode NAT; Dell UNIX keeps its address 192.168.1.111 (host name amnesia) with gateway and DNS 192.168.1.1, and this computer is 192.168.1.10. telnet localhost 22023 reaches Dell UNIX; the port listens on this computer only.
The clock is the host's local time; Dell UNIX shows 2026 fine.
Mouse: a Microsoft serial mouse on COM1. QEMU grabs it when you click in the window; Ctrl-Alt-G lets it go.
QEMU halts the CPU while Dell UNIX is idle, so an idle VM uses about 1% of a host core.
Shut down with Shutdown > Powerdown in Dell's menu, or shutdown -y -g0 -i0 as root, and wait for "Press any key to reboot" before quitting QEMU.
macOS: if the binary is blocked, run xattr -cr . in this folder.

AI agents (macOS, Linux): in this folder, claude mcp add lemu -s user -- "$PWD/bin/macos-arm64/qemu-system-i386" -mcp-bridge (on Linux bin/linux-x86_64 or bin/linux-aarch64) lets an agent such as Claude Code see the screen and use the keyboard, mouse and serial ports of the running VM. See MCP.md in the QEMU source.
QEMU source: https://github.com/tenox7/lemu branch master (GPLv2, see bin/COPYING). qemu-system-i386 --version names the commit. LEMU added the WD8003E card and two Cirrus fixes for XFree86 for this kit.
