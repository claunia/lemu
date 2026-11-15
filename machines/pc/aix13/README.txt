IBM AIX PS/2 1.3 (PTF 0024) for x86 on QEMU's PC (486, 16 MB, i440FX, S3 86C928 with 1 MB)

Run ./run.sh (macOS 26+ arm64, Linux x86_64/aarch64 with glibc 2.34+ and X11/XWayland) or run.cmd (Windows 10+ x64).
AIX boots to an X11 login box (xdm). Log in as root, no password: type root, then press Enter at Password:. telnet localhost 1323 works too.
rsh to localhost port 13514 runs commands as root (no .rhosts needed).
X11: after the login you get the Motif window manager and an "AIX Shell" window; click a window to type in it. In the AIX Shell, xdt & starts the AIXwindows Desktop, and exit logs out. F1 instead of Enter at Password: gives a failsafe session (one aixterm). Ctrl-Alt-Backspace restarts X.
The text console ("Console login:") is on Alt + Right Ctrl, AIX's key for the next virtual terminal; press it again to go back to X. Without a Right Ctrl key (Mac keyboards), type sendkey alt-ctrl_r in the QEMU monitor in the terminal.
xdm was built from IBM's X11R5 sample source (/usr/lpp/X11/samples/clients/xdm) with two fixes, in verify.c and Login.c (IBM's files are the .orig ones). To boot to the text console instead: cp /aixps2/inittab.orig /aixps2/inittab and reboot; xinit (twm) or xinit -mwm (Motif) then starts X.
X runs 1024x768 with 256 colours on the S3 card. That is the biggest mode IBM's X server has, and its S3 driver takes the card only with exactly 1 MB of video memory (vram-size-mb=1 in run.sh).
Installed: the Base Operating System and 28 program products, among them TCP/IP, NFS, X Windows, AIXwindows (Motif) and its Desktop, Xstation Manager, the MetaWare High C compiler (cc), Advanced Development Tools, SCCS, INed, Message Handler, Text Formatting System, games and the man pages. All are updated to PTF 0024 (1995).
Extra apps, built with the MetaWare compiler, in /usr/local/bin (already in the PATH): bash 1.14.7, gmake (GNU make 3.79.1), gzip 1.2.4, xv 3.10a (image viewer), Mosaic (NCSA Mosaic 2.7b5, web browser), doom (DOOM 1.10 with the shareware WAD, in a 640x400 window, doom -1 for 320x200, no sound), gcc 1.39 (simple C only) and hcc. Also 44 X11R5 sample programs from IBM's sample source, among them xeyes, xgas, ico, maze, muncher, plaid, puzzle, xcalc, xterm, xman and x11perf. From telnet or rsh, start X programs with DISPLAY=unix:0.
Web: Mosaic & in the AIX Shell. It does http, ftp and gopher with GIF, JPEG and PNG pictures, but no HTTPS, so it needs sites that still serve plain HTTP, such as http://theoldnet.com/ (old sites from the Wayback Machine) and gopher://gopher.floodgap.com/. Table Support (Options menu) is off: Mosaic 2.7b5 puts only text in table cells.
hcc is cc with fixes for 5 bugs of the MetaWare compiler and assembler that break ported code (inline strlen, float results popped off the FPU stack, a register used before it is loaded, octal escapes in strings, relocation order): build ported code with make CC=hcc. bash scripts first print a harmless "bad substitution" (IBM's /etc/profile sets ENV for ksh). Packages, sources and notes: https://files.42718.net/mirror/tenox/os/aix/1.3ps2/apps/
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
