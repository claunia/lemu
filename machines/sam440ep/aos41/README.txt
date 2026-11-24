AmigaOS 4.1 on QEMU's ACube Sam440ep (PowerPC 440EP, 512 MB, Radeon, SiI3114 SATA, USB keyboard and mouse, 3Com 3C905C Ethernet, CS4281 sound)

Run ./run.sh (macOS 26+ arm64, Linux x86_64/aarch64 with glibc 2.34+ and X11/XWayland) or run.cmd (Windows 10+ x64).
The Sam440ep starts U-Boot (fw/u-boot-sam440ep.bin, built from source), which runs the Radeon's VGA BIOS (fw/vgabios-ati.bin), shows its boot menu and boots the disk after 5 seconds. In about a minute the Workbench comes up at 1024x768.
Click in the QEMU window to grab the mouse; Ctrl-Alt-G (Ctrl-Option-G on a Mac) lets it go.
Extra QEMU options are passed through, e.g. ./run.sh -display vnc=127.0.0.1:1.
The terminal holds the QEMU monitor. quit there stops QEMU at once.
CD-ROM: in the monitor, change cd0 file.iso.
Network is QEMU user mode NAT. The interface ETH3COM (eth3com.device) gets its address by DHCP; ping 10.0.2.2 reaches the host side. The card sits in PCI slot 07: AmigaOS 4.1 never enables the interrupt of slot 06.
Sound: AHI with cs4281.audio, unit 0 set to the CS4281 HiFi 16 bit stereo mode. Prefs/AHI plays a test sound.
The disk writes are slow: the file system flushes the disk after almost every write, so copying many files (an install) takes a long time.
Shut down: close the window or type quit in the terminal when the disk is idle.
macOS: if the binary is blocked, run xattr -cr . in this folder.

AI agents (macOS, Linux): in this folder, claude mcp add lemu -s user -- "$PWD/bin/macos-arm64/qemu-system-ppc" -mcp-bridge (on Linux bin/linux-x86_64 or bin/linux-aarch64) lets an agent such as Claude Code see the screen and use the keyboard, mouse and serial ports of the running VM. See MCP.md in the QEMU source.
QEMU source: https://github.com/tenox7/lemu branch master (GPLv2, see bin/COPYING). qemu-system-ppc --version names the commit. The U-Boot source is ACube's u-boot-2015.d with the patch in machines/sam440ep/firmware there (GPLv2).
