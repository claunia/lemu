@echo off
cd /d "%~dp0"
rem HP-UX 11.00 (B.11.00) on an HP B160L (PA-7300LC): four CPUs, 512 MB RAM. See run.sh.
bin\windows-x86_64\qemu-system-hppa.exe ^
 -name "HP-UX 11.00 PA-RISC B160L" ^
 -M B160L -smp 4 -m 512 ^
 -accel tcg,thread=multi,tb-size=512 ^
 -boot c -boot menu=off ^
 -drive if=scsi,bus=0,index=6,file=hpux1100.qcow2,format=qcow2 ^
 -nic user,model=tulip,net=192.168.1.0/24,host=192.168.1.2,dns=192.168.1.1,hostfwd=tcp:127.0.0.1:11023-192.168.1.110:23 ^
 -monitor stdio ^
 %*
