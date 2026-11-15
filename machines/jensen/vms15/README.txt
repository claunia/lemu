OpenVMS AXP V1.5-1H1 for DEC Alpha on QEMU's DEC 2000 Model 300 AXP (Jensen)

Run ./run.sh (macOS 26+ arm64, Linux x86_64/aarch64 with glibc 2.34+ and X11/XWayland) or run.cmd (Windows 10+ x64).
The window is the VMS console (OPA0). VMS boots by itself in about 20 s. Log in as SYSTEM, password systempassword. SYSTEST (password systestpassword) and FIELD (password fieldpassword) are there too. Extra QEMU options are passed through, e.g. ./run.sh -display vnc=:1.
Installed: OpenVMS AXP V1.5-1H1 (1993), DEC TCP/IP Services for OpenVMS (UCX) V3.0 and DECwindows Motif V1.1, with hobbyist licenses.
Shut down: @SYS$SYSTEM:SHUTDOWN 0 TEST NO YES LATER NO NONE, wait for SYSTEM SHUTDOWN COMPLETE, then close the window. Reboot: @SYS$SYSTEM:SHUTDOWN 0 TEST NO YES SOON YES NONE. In a telnet session it first asks "Do you really want to shutdown node VMS15": answer YES.
The clock runs 28 years behind: the firmware rejects current years, and the 1998 date keeps the licenses valid.
Network: a DEC DE422 EISA card (ERA0 in VMS, RE0 in UCX) on QEMU user mode NAT. VMS has the fixed address 192.168.1.15 (node VMS15). QEMU answers as 192.168.1.1 (gateway and DNS), and this computer is 192.168.1.10.
telnet: telnet 127.0.0.1 1523. rsh: 127.0.0.1:1514 goes to VMS's rsh server, e.g. rsh-mcp (github.com/tenox7/rsh-mcp) with host 127.0.0.1, port 1514, user SYSTEM, shell login. It is unauthenticated: it asks for no password, so anyone who can reach its port can run any command as SYSTEM. Both listen on 127.0.0.1 only, so other computers can't reach them.
UCX's rsh server cuts a command line at about 128 characters and has no rcp. Use telnet for longer commands. UCX also has an FTP server (not forwarded).
DECwindows: the Jensen's graphics card for DECwindows (a Compaq QVision) is not emulated, so DECwindows runs on your X server over TCP/IP. DECwindows Motif V1.1 has no XDMCP. SYS$MANAGER:XLOGIN.COM puts the DECwindows login box on an X display instead:
 1. Let your X server take TCP connections. XQuartz: Settings, Security, Allow connections from network clients, then restart XQuartz. Xvfb: -listen tcp -noreset.
 2. Let VMS in: xhost +localhost (QEMU's NAT connects from 127.0.0.1).
 3. In VMS (console or telnet): @SYS$MANAGER:XLOGIN 192.168.1.10:0 (or :1 for display 1).
 4. Log in to DECwindows. After End Session the login box comes back, until your X server stops.
 From a telnet session you can also start single programs: SET DISPLAY/CREATE/NODE=192.168.1.10/TRANSPORT=TCPIP/SERVER=0, then RUN SYS$SYSTEM:DECW$CLOCK.
QEMU monitor: Ctrl-Alt-2 (back with Ctrl-Alt-1). The CD-ROM (DKA400, an RRD42) and the floppy are empty: change cd0 file.iso, change fd0 file.img. To use a CD in VMS: MOUNT/OVERRIDE=IDENTIFICATION DKA400:.
macOS: if the binary is blocked, run xattr -cr . in this folder.

AI agents (macOS, Linux): in this folder, claude mcp add lemu -s user -- "$PWD/bin/macos-arm64/qemu-system-alpha" -mcp-bridge (on Linux bin/linux-x86_64 or bin/linux-aarch64) lets an agent such as Claude Code see the screen and use the keyboard, mouse and serial ports of the running VM. See MCP.md in the QEMU source.
QEMU source: https://github.com/tenox7/lemu branch master (GPLv2, see bin/COPYING). qemu-system-alpha --version names the commit.
