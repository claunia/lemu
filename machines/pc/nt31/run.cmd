@echo off
cd /d "%~dp0"
bin\windows-x86_64\qemu-system-i386.exe ^
 -name "Windows NT 3.1 X86 PC" ^
 -M pc-i440fx-11.1 -cpu pentium -m 1024 ^
 -bios fw/bios-nt31.bin ^
 -rtc base=localtime ^
 -vga none -device s3-928,vram-size-mb=4,romfile=fw/928-n9.bin ^
 -drive file=nt31.qcow2,if=none,id=hd0,format=qcow2 ^
 -device ide-hd,drive=hd0,bus=ide.0,unit=0,cyls=8128,heads=16,secs=63,lcyls=1024,lheads=127,lsecs=63 ^
 -drive if=floppy,index=0,id=fd0 ^
 -netdev user,id=n0,hostfwd=tcp:127.0.0.1:34514-:514,hostfwd=tcp:127.0.0.1:34389-:3389 -device ne2k_isa,iobase=0x300,irq=5,netdev=n0 ^
 -monitor stdio ^
 %*
