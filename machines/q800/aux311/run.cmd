@echo off
cd /d "%~dp0"
rem A/UX 3.1.1 on a Quadra 800 (q800): 68040, 128 MB RAM, 1152x870 screen with 256 colours. See run.sh.
bin\windows-x86_64\qemu-system-m68k.exe ^
 -name "A/UX 3.1.1 68040 Quadra 800" ^
 -M q800 -m 128 -bios Quadra800.rom ^
 -g 1152x870x8 ^
 -drive file=pram.img,format=raw,if=mtd ^
 -drive file=aux311.qcow2,if=none,id=hd0,format=qcow2 ^
 -device scsi-hd,scsi-id=0,drive=hd0 ^
 -nic user,model=dp83932,net=192.168.1.0/24,host=192.168.1.10,dns=192.168.1.1,hostfwd=tcp:127.0.0.1:31123-192.168.1.2:23,hostfwd=tcp:127.0.0.1:31514-192.168.1.2:514 ^
 -monitor stdio ^
 %*
