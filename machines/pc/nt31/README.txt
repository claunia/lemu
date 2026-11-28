Windows NT 3.1 (build 511, SP3) for x86 on QEMU's PC (Pentium, i440FX, 1 GB)

Run ./run.sh (macOS 26+ arm64, Linux x86_64/aarch64 with glibc 2.34+ and X11/XWayland) or run.cmd (Windows 10+ x64).
NT logs on automatically as Administrator (password: password). Extra QEMU options are passed through, e.g. ./run.sh -display vnc=:1.
The QEMU monitor is in the terminal: sendkey ctrl-alt-delete, change fd0 file.img.
Mouse: the vmmouse driver (LEMU machines/vmmouse) is installed, so the pointer moves in and out of the window without a grab. NT 3.1 has no wheel support.
The screen is 1280x1024 with 256 colours (S3 86C928, 4 MB, NT's own S3 driver). NT is in C:\WINNT (NTFS).
fw/bios-nt31.bin is SeaBIOS with one value changed (INT 15h function 88h says 15 MB), so NT 3.1's loader sees all 1 GB, not 64 MB. More than 1 GB hangs the loader.
fw/928-n9.bin is the card's VGA BIOS: NT's S3 driver sets every mode through it.
Keep the disk geometry in run.sh: NT 3.1's disk driver reads the BIOS head count as a signed byte.
No CD-ROM: NT 3.1 can't use QEMU's IDE CD-ROM or its SCSI cards. Move files with rcp (below) or a floppy image.
NT's logo stays on the desktop after logon until a window is moved over it.
QEMU halts the CPU while NT is idle, so an idle VM uses little of a host core.
The clock is the host's local time. Network is QEMU user mode NAT (NE2000). NT 3.1 has no DHCP client: NT has 10.0.2.15, gateway 10.0.2.2, DNS 10.0.2.3.
Compiler: the Win32 SDK for NT 3.1 (July 1993) in C:\MSTOOLS. The NT 3.1 SDK compilers need -Od, not -Ox.
DDK: the Windows NT 3.1 DDK (November 1993) is in C:\DDK. To build a driver, open Program Manager, group Windows NT DDK, Free Build Environment, cd to a folder with a sources file and type build -cef. The driver lands in C:\DDK\lib\i386\free.
DDK from rsh or a batch job: first type set PROCESSOR_ARCHITECTURE=x86, set TMP=C:\TEMP, set TEMP=C:\TEMP and call C:\DDK\bin\setenv.bat C:\DDK free. NT 3.1 has no cd /d.
Build on the local disk, not on a network share: NT 3.1 damages files there.
rsh: the ntrshd service (rsh and rcp server, github.com/tenox7/ntrshd) starts at boot and runs commands as Administrator. It is unauthenticated: it asks for no password, so anyone who can reach its port can run any command as Administrator. 127.0.0.1:34514 on this computer goes to it, e.g. rsh-mcp (github.com/tenox7/rsh-mcp) with host 127.0.0.1 and port 34514. 127.0.0.1:34389 goes to port 3389 (RDP) in NT, for an RDP server you install there. Both listen on 127.0.0.1 only, so other computers can't reach them.
macOS: if the binary is blocked, run xattr -cr . in this folder.

AI agents (macOS, Linux): in this folder, claude mcp add lemu -s user -- "$PWD/bin/macos-arm64/qemu-system-i386" -mcp-bridge (on Linux bin/linux-x86_64 or bin/linux-aarch64) lets an agent such as Claude Code see the screen and use the keyboard, mouse and serial ports of the running VM. See MCP.md in the QEMU source.
QEMU source: https://github.com/tenox7/lemu branch master (GPLv2, see bin/COPYING). qemu-system-i386 --version names the commit.
