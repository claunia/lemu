@echo off
cd /d "%~dp0"
rem The clock runs 28 years back (same weekdays): AIX PS/2 shows 1970 for 2026.
for /f %%d in ('powershell -NoProfile -Command "(Get-Date).AddYears(-28).ToString('yyyy-MM-ddTHH:mm:ss')"') do set base=%%d
bin\windows-x86_64\qemu-system-i386.exe ^
 -name "AIX PS/2 1.3 486 PC" ^
 -M pc-i440fx-11.1,fd-bootchk=off -cpu 486 -m 16 ^
 -global isa-i8259.edge-irq-withdrawal=on ^
 -global isa-fdc.cmd-drops-irq=on -global isa-fdc.seek-latency-us=1000 ^
 -global isa-fdc.fallback=144 ^
 -rtc base=%base% ^
 -vga none -device s3-928,vram-size-mb=1,romfile=fw/928-n9.bin ^
 -drive file=aix13.qcow2,if=none,id=hd0,format=qcow2 ^
 -device ide-hd,drive=hd0,bus=ide.0,unit=0,cyls=1024,heads=16,secs=63,bios-chs-trans=none ^
 -drive if=floppy,index=0,id=fd0 ^
 -boot order=c,menu=off ^
 -netdev user,id=n0,hostfwd=tcp:127.0.0.1:1323-:23,hostfwd=tcp:127.0.0.1:13514-:514 -device pcnet,netdev=n0,romfile= ^
 -monitor stdio ^
 %*
