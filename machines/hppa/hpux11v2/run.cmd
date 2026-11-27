@echo off
cd /d "%~dp0"
rem HP-UX 11i v2 (B.11.23, 64-bit) on an HP C3700 (PA-8700): one CPU, 2 GB RAM. See run.sh.
bin\windows-x86_64\qemu-system-hppa.exe ^
 -name "HP-UX 11i v2 PA-RISC C3700" ^
 -M C3700 -smp 1 -m 2048 ^
 -accel tcg,thread=multi,tb-size=512 ^
 -boot c -boot menu=off ^
 -drive if=scsi,bus=0,index=6,file=hpux11v2.qcow2,format=qcow2 ^
 -nic user,model=tulip,hostfwd=tcp:127.0.0.1:11223-10.0.2.15:23 ^
 -device artist-pci ^
 -serial mon:stdio ^
 %*
