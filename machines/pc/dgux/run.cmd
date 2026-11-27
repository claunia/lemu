@echo off
cd /d "%~dp0"
bin\windows-x86_64\qemu-system-i386.exe ^
 -name "DG/UX R4.20MU07 Pentium PC" ^
 -M pc-i440fx-11.2 -cpu pentium -m 128 ^
 -rtc base=utc -global mc146818rtc.pf-without-pie=on ^
 -vga cirrus ^
 -device lsi53c810,romfile=fw/NCR307-810.BIN,id=scsi0,addr=0x0a ^
 -drive file=dgux.qcow2,if=none,id=hd0,format=qcow2 ^
 -device scsi-hd,drive=hd0,bus=scsi0.0,scsi-id=0 ^
 -drive file=068-600216-07__DG-UX_with_X_Intel_R4.20-R4.20MU07__1995-1999.iso,if=none,id=cd0,media=cdrom,readonly=on ^
 -device scsi-cd,drive=cd0,bus=scsi0.0,scsi-id=4 ^
 -netdev user,id=n0,hostfwd=tcp:127.0.0.1:23023-:23,hostfwd=tcp:127.0.0.1:23514-:514 ^
 -device tulip,netdev=n0,srom-name=DE500-BA,bios-bus-master=on,addr=0x0b ^
 -monitor stdio ^
 %*
