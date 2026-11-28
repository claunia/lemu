A/UX 3.1.1 on QEMU's Quadra 800 (68040, 128 MB, 1152x870 with 256 colours, SCSI disk, SONIC Ethernet)

Run ./run.sh (macOS 26+ arm64, Linux x86_64/aarch64 with glibc 2.34+ and X11/XWayland) or run.cmd (Windows 10+ x64).
The Quadra starts from its ROM (Quadra800.rom, Apple's), the Mac OS partition starts A/UX, and within a minute the A/UX Finder comes up, logged in as root.
Shells: CommandShell in the Apple menu, telnet localhost 31123, or rsh port 31514 on this host. Log in as root, no password; at TERM = (vt100) press Return. Both ports listen on this computer only.
Installed in /usr/local/bin: gcc 2.8.1 (gcc, g++; gcc 2.7.2 is gcc272), GNU make 3.79 (gmake), bash, vim 5.8, MicroEMACS 4.00 (me), joe, pico, pine, wget, curl, ncftp 3.2.6, screen and top. The X11 programs are in /usr/bin/X11. /opt/jagubox and /opt/nleymann hold archives of more A/UX software.
X11: start MacX, an X server that runs as a Mac program, from the Apple menu, and answer OK when it asks about memory. Then in a shell: DISPLAY=127.0.0.1:0; export DISPLAY; /usr/bin/X11/xterm &
Web: Archaic (github.com/tenox7/archaic), a browser for WRP (github.com/tenox7/wrp). Start wrp on this computer (port 8080), then in CommandShell: /usr/local/bin/archaic. It finds WRP at 192.168.1.10:8080, this computer as A/UX sees it. Type an address in its box and press Return; Cmd-Q quits.
SimCity for X11 (DUX Software, 1994), built here from the Micropolis source: with MacX running and DISPLAY set, cd /usr/SimCity; ./SimCity, or ./SimCity cities/kowloon.cty to start with a city.
Click in the QEMU window to grab the mouse; Ctrl-Alt-G (Ctrl-Option-G on a Mac) lets it go.
Extra QEMU options are passed through, e.g. ./run.sh -display vnc=127.0.0.1:1.
The terminal holds the QEMU monitor. quit there stops QEMU at once.
Network is QEMU user mode NAT on 192.168.1.0/24: A/UX is 192.168.1.2 (hostname aux), the host side is 192.168.1.10, and QEMU answers DNS at 192.168.1.1. A/UX has no default route, so it reaches only 192.168.1.x. As root, /usr/etc/route add default 192.168.1.1 1 lets it out to the Internet until the next boot.
Shut down with Special > Shut Down in the Finder, or /etc/shutdown -y -g0 -i0 now as root. When the screen says "You may now switch off your Macintosh safely", close the window or type quit in the terminal: QEMU does not exit by itself.
macOS: if the binary is blocked, run xattr -cr . in this folder.

AI agents (macOS, Linux): in this folder, claude mcp add lemu -s user -- "$PWD/bin/macos-arm64/qemu-system-m68k" -mcp-bridge (on Linux bin/linux-x86_64 or bin/linux-aarch64) lets an agent such as Claude Code see the screen and use the keyboard, mouse and serial ports of the running VM. See MCP.md in the QEMU source.
QEMU source: https://github.com/tenox7/lemu branch master (GPLv2, see bin/COPYING). qemu-system-m68k --version names the commit.
