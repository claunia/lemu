@echo off
cd /d "%~dp0"
bin\windows-x86_64\qemu-system-i386.exe ^
 -name "SINIX-Z 5.42 Pentium II PC" ^
 -M pc-i440fx-11.2 -cpu pentium2 -m 128 ^
 -global isa-i8259-pic.edge-irqs=0x800 ^
 -rtc base=localtime ^
 -vga none -device cirrus-vga-gd5436 ^
 -device aha154x,id=scsi0,romfile=fw/aha1540b320_330.bin ^
 -drive file=sinixz.qcow2,if=none,id=hd0,format=qcow2 ^
 -device scsi-hd,drive=hd0,bus=scsi0.0,scsi-id=0 ^
 -drive if=none,id=cd0,media=cdrom,readonly=on ^
 -device scsi-cd,drive=cd0,bus=scsi0.0,scsi-id=4 ^
 -serial msmouse ^
 -netdev user,id=n0,hostfwd=tcp:127.0.0.1:24023-:23,hostfwd=tcp:127.0.0.1:24514-:514 ^
 -device wd8003,netdev=n0,mac=00:00:c0:12:34:56,iobase=0x240,irq=5 ^
 -monitor stdio ^
 %*
