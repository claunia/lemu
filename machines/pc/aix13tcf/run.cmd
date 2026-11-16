@echo off
cd /d "%~dp0"
rem Two VMs, one AIX PS/2 TCF cluster. Site 2 (aixps2b) starts in the
rem background; its card is only on a link to site 1 (127.0.0.1:24600) and it
rem connects again until site 1 listens. Site 1 (aixps2) runs here, with the
rem QEMU monitor in this window; a hub joins its card, QEMU's NAT and the link.
rem The clock runs 28 years back (same weekdays): AIX PS/2 shows 1970 for 2026.
for /f %%d in ('powershell -NoProfile -Command "(Get-Date).AddYears(-28).ToString('yyyy-MM-ddTHH:mm:ss')"') do set base=%%d
set pc=-M pc-i440fx-11.1,fd-bootchk=off -cpu 486 -m 16 -global isa-i8259.edge-irq-withdrawal=on -global isa-fdc.cmd-drops-irq=on -global isa-fdc.seek-latency-us=1000 -global isa-fdc.fallback=144 -rtc base=%base% -vga none -device s3-928,vram-size-mb=1,romfile=fw/928-n9.bin -drive if=floppy,index=0,id=fd0 -boot order=c,menu=off
set hd=if=none,id=hd0,format=qcow2 -device ide-hd,drive=hd0,bus=ide.0,unit=0,cyls=1024,heads=16,secs=63,bios-chs-trans=none
start "" /b bin\windows-x86_64\qemu-system-i386.exe -name "AIX PS/2 1.3 TCF site 2 aixps2b" %pc% ^
 -drive file=site2.qcow2,%hd% ^
 -netdev stream,id=link,server=off,addr.type=inet,addr.host=127.0.0.1,addr.port=24600,reconnect-ms=1000 ^
 -device pcnet,netdev=link,romfile=,mac=52:54:00:12:34:57
bin\windows-x86_64\qemu-system-i386.exe -name "AIX PS/2 1.3 TCF site 1 aixps2" %pc% ^
 -drive file=site1.qcow2,%hd% ^
 -netdev user,id=nat,hostfwd=tcp:127.0.0.1:2301-10.0.2.15:23,hostfwd=tcp:127.0.0.1:2302-10.0.2.16:23,hostfwd=tcp:127.0.0.1:51401-10.0.2.15:514,hostfwd=tcp:127.0.0.1:51402-10.0.2.16:514 ^
 -netdev stream,id=link,server=on,addr.type=inet,addr.host=127.0.0.1,addr.port=24600 ^
 -netdev hubport,id=hub-nat,hubid=0,netdev=nat ^
 -netdev hubport,id=hub-link,hubid=0,netdev=link ^
 -netdev hubport,id=hub-card,hubid=0 ^
 -device pcnet,netdev=hub-card,romfile=,mac=52:54:00:12:34:56 ^
 -monitor stdio ^
 %*
