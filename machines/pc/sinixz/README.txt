SINIX-Z 5.42 B20 for Intel (Siemens Nixdorf SVR4, 1996) on QEMU's PC (Pentium II, 128 MB, i440FX, Adaptec AHA-1540B SCSI, Cirrus CL-GD5436 with 2 MB, WD8003E Ethernet, Microsoft serial mouse)

Run ./run.sh (macOS 26+ arm64, Linux x86_64/aarch64 with glibc 2.34+ and X11/XWayland) or run.cmd (Windows 10+ x64).
SINIX-Z boots by itself to the X login (xdm). The Adaptec BIOS shows first, then the SINIX boot menu waits 7 seconds. Log in as root, password password. Click into the Login field before you type: the X login takes keys only after a click.
X: SINIX/windows at 1152x864 with 256 colours, the X server's own Cirrus 5436 driver (set up in /usr/lib/X11/server/config).
Network: QEMU user mode NAT. SINIX-Z keeps its fixed address 10.0.2.15 (host name sinix), and this computer is 10.0.2.2. telnet localhost 24023 reaches SINIX-Z, and port 24514 is its rsh port (514). These ports listen on this computer only. /.rhosts has "+ +": anyone who can reach port 24514 gets in as root without a password.
CD: the SCSI CD drive (SCSI ID 4) is empty. To load an .iso, type in the QEMU monitor (the terminal): change cd0 FILE.iso.
Mouse: Microsoft serial mouse on COM1. QEMU grabs it when you click in the window; Ctrl-Alt-G lets it go.
Shut down as root with shutdown -y -g0 -i0. Quit QEMU when the console says "Reboot the system now."
macOS: if the binary is blocked, run xattr -cr . in this folder.
The same install for 86Box: sinixz542-86box.rar.

AI agents (macOS, Linux): in this folder, claude mcp add lemu -s user -- "$PWD/bin/macos-arm64/qemu-system-i386" -mcp-bridge (on Linux bin/linux-x86_64 or bin/linux-aarch64) lets an agent such as Claude Code see the screen and use the keyboard, mouse and serial ports of the running VM. See MCP.md in the QEMU source.
QEMU source: https://github.com/tenox7/lemu branch master (GPLv2, see bin/COPYING). qemu-system-i386 --version names the commit. LEMU changed for this kit: the Adaptec AHA-1540B ISA SCSI adapter (aha154x, new), edge-irqs on the PIC to keep IRQ 11 edge-triggered for it, and the Cirrus GD5436 (cirrus-vga-gd5436) with its four linear apertures, where the SINIX X server finds the BitBLT registers. fw/aha1540b320_330.bin is Adaptec's AHA-1540B BIOS 3.20 for port 330h.
