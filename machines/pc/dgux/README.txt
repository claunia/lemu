DG/UX R4.20MU07 for Intel (Data General, 1999) on QEMU's PC (Pentium, 128 MB, i440FX, NCR 53C810 SCSI, Cirrus CL-GD5446 with 4 MB, DEC 21143 Ethernet)

Run ./run.sh (macOS 26+ arm64, Linux x86_64/aarch64 with glibc 2.34+ and X11/XWayland) or run.cmd (Windows 10+ x64).
DG/UX boots by itself to the X login (xdm) in about 3 minutes: the boot loader waits 15 seconds (Escape stops it), then the text console shows the boot until X starts. Log in as root, no password: type root, press Enter, then Enter again at the password. telnet localhost 23023 works too.
X runs 1280x1024 with 256 colours: XFree86 3.2's SVGA server for the Cirrus (/usr/opt/X11/bin/Xdg.SVGA, set up in /var/X11/Xserver/XdgConfig). XdgConfig.mach32 there is the setup for 86Box's ATI Mach32.
Programs: type the name in an xterm.
- coreldraw: CorelDRAW 3.0 for UnixWare. Also corelchart, corelpaint, corelshow, coreltrace and corelmosaic.
- maker: FrameMaker 3.0X (Intel ABI). Also imaker and viewer.
- xwp: WordPerfect 8 for X (the Solaris x86 build). A "File IO Error" box comes up at start: click OK.
- wp: WordPerfect 8 in text mode (the SCO build).
- simcity: SimCity for X11 by DUX Software (Micropolis 4.0 inside).
- archaic: Archaic, a small web browser for WRP (https://github.com/tenox7/archaic). Start WRP (https://github.com/tenox7/wrp) on this computer first: Archaic looks for it at http://10.0.2.2:8080/.
- gcc 3.0.4 and GNU tools are in /usr/local/bin.
The screen has 256 colours. If a program can't get its colours (SimCity says "Couldn't open X11 display"), close the other big programs and try again.
Network: QEMU user mode NAT. DG/UX keeps its fixed address 10.0.2.15 (host name av86box), and this computer is 10.0.2.2. telnet localhost 23023 reaches DG/UX, and port 23514 is its rsh port (514). These ports listen on this computer only. root has no password: anyone who can reach them gets in.
CD: the DG/UX R4.20MU07 install CD is in the SCSI CD drive (SCSI ID 4). At the DG/UX boot prompt it is sd(npsc(pci(),a),4). Another .iso: in the QEMU monitor (the terminal), change cd0 FILE.iso.
The clock is the host's UTC time.
Mouse: PS/2. QEMU grabs it when you click in the window; Ctrl-Alt-G lets it go.
DG/UX does not halt the CPU when it is idle, so QEMU uses one host core all the time.
Shut down with /sbin/init 0 as root. The PC then restarts: quit QEMU when the boot loader says "DG/UX System will boot in 15 seconds", or let it start DG/UX again (that is how to restart it). init 6 restarts DG/UX in single-user mode: type /sbin/init 3 there.
macOS: if the binary is blocked, run xattr -cr . in this folder.
The same install for 86Box: DGUX-R4.20MU07-86Box.tar.lz.

AI agents (macOS, Linux): in this folder, claude mcp add lemu -s user -- "$PWD/bin/macos-arm64/qemu-system-i386" -mcp-bridge (on Linux bin/linux-x86_64 or bin/linux-aarch64) lets an agent such as Claude Code see the screen and use the keyboard, mouse and serial ports of the running VM. See MCP.md in the QEMU source.
QEMU source: https://github.com/tenox7/lemu branch master (GPLv2, see bin/COPYING). qemu-system-i386 --version names the commit. LEMU changed for this kit: the 21143's board name (srom-name), bus mastering at reset (bios-bus-master), the PS/2 mouse's wrap mode, and the Cirrus 8-bit modes with a doubled pixel clock that X uses for 1280x1024. fw/NCR307-810.BIN is NCR's SDMS 3.07 SCSI BIOS with the 53C810's PCI ID.
