@echo off
cd /d "%~dp0"
for /f "usebackq delims=" %%t in (`powershell -NoProfile -Command "(Get-Date).AddYears(-28).ToString('s')"`) do set rtc=%%t
bin\windows-x86_64\qemu-system-alpha.exe ^
 -name "OpenVMS AXP 1.5-1H1 ALPHA JENSEN" ^
 -M jensen,rtc-ram-file=rtc.bin,board-id=off -m 128 ^
 -rtc base=%rtc% ^
 -drive if=pflash,index=0,format=raw,file=feprom0.bin ^
 -drive if=pflash,index=1,format=raw,file=feprom1.bin ^
 -device jensen-aha1742,id=scsi0 ^
 -drive if=none,id=hd0,file=vms15.qcow2,format=qcow2 ^
 -device scsi-hd,bus=scsi0.0,scsi-id=0,drive=hd0,quirk_mode_page_rw_error_vms=on,quirk_mode_page_format_device=on ^
 -drive if=none,id=cd0,media=cdrom,readonly=on ^
 -device "scsi-cd,bus=scsi0.0,scsi-id=4,drive=cd0,physical_block_size=512,quirk_mode_page_set_block_size=on,quirk_mode_sense_rom_use_dbd=on,quirk_mode_page_rw_error_vms=on,quirk_mode_sense_rom_wp=on,scsi_version=2,vendor=DEC,product=RRD42   (C) DEC,ver=4.5d" ^
 -drive if=floppy,index=0,id=fd0 ^
 -netdev user,id=n0,net=192.168.1.0/24,host=192.168.1.10,dns=192.168.1.1,hostfwd=tcp:127.0.0.1:1523-192.168.1.15:23,hostfwd=tcp:127.0.0.1:1514-192.168.1.15:514 -device jensen-de422,netdev=n0 ^
 %*
