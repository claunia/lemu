AIX 7.3 (7300-03-00) on QEMU's pSeries (POWER9 logical partition, 4 CPUs in 2 NUMA nodes, virtual SCSI disk)

Run ./run.sh (macOS 26+ arm64, Linux x86_64/aarch64 with glibc 2.34+) or run.cmd (Windows 10+ x64).
AIX runs on the serial console in the terminal, which also holds the QEMU monitor: Ctrl-A c switches between them, Ctrl-A x quits.
Log in as root (no password) at the console login prompt, telnet localhost 7323 or rsh localhost port 7314. ssh localhost -p 7322 works once root has a password.
The VM has 8 GB of memory. AIX takes about 3 minutes to boot.
It looks like a PowerVM partition: lparstat -i, lssrad -av and ASO (Active System Optimizer) work. The boot disk is hdisk1.
AIX 7.3 needs the aix-compat and aix-papr machine options of this QEMU (from github.com/spuhpointer/qemu, now in LEMU).
GNU tools are RPMs from IBM's AIX Toolbox in /opt/freeware, with dnf: gcc and g++ 10.3, bash 5.2, GNU make 4.4 (gmake), gdb 13.1, GNU tar (gtar), gzip, bzip2, xz, zip, unzip, less, autoconf, wget and curl. All are in /opt/freeware/bin, and each has a name in /usr/bin (gmake, gtar). /usr/bin/make, tar, bash and gzip are AIX's own.
More Toolbox packages: change cd0 to the Toolbox ISO (Toolbox_for_Linux_Apps_Common_7.1-7.3) in the monitor, then mount -v udfs -o ro /dev/cd0 /mnt/toolbox and dnf install NAME.
Extra QEMU options are passed through.
The CD-ROM is empty: change cd0 file.iso in the monitor.
Network is QEMU user mode NAT; AIX is 10.0.2.15 by DHCP with gateway 10.0.2.2 and DNS 10.0.2.3.
The time zone is US Pacific (PST8PDT). Change it with chtz, then reboot.
Run shutdown -F: QEMU quits when AIX has halted.
macOS: if the binary is blocked, run xattr -cr . in this folder.

AI agents (macOS, Linux): in this folder, claude mcp add lemu -s user -- "$PWD/bin/macos-arm64/qemu-system-ppc64" -mcp-bridge (on Linux bin/linux-x86_64 or bin/linux-aarch64) lets an agent such as Claude Code see the screen and use the keyboard, mouse and serial ports of the running VM. See MCP.md in the QEMU source.
QEMU source: https://github.com/tenox7/lemu branch master (GPLv2, see bin/COPYING). qemu-system-ppc64 --version names the commit.
