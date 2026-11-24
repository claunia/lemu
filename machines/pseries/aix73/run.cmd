@echo off
cd /d "%~dp0"
rem AIX 7.3 boots only with aix-compat; aix-papr makes the partition look like
rem PowerVM. nvramrc raises SLOF's PREP load limit and sets the machine ID
rem (see run.sh). Keep x-aix-vscsi-id: AIX finds its boot disk again by it.
bin\windows-x86_64\qemu-system-ppc64.exe ^
 -name "AIX 7.3 POWER PSERIES" ^
 -M pseries,aix-compat=on,aix-papr=on,ic-mode=xics,cap-cfpc=broken,cap-sbbc=broken,cap-ibs=broken,cap-ccf-assist=off ^
 -cpu POWER9 -smp 4,sockets=2,cores=2,threads=1 -m 8G ^
 -object memory-backend-ram,id=ram0,size=4G ^
 -object memory-backend-ram,id=ram1,size=4G ^
 -numa node,nodeid=0,cpus=0-1,memdev=ram0 ^
 -numa node,nodeid=1,cpus=2-3,memdev=ram1 ^
 -serial mon:stdio ^
 -nographic ^
 -vga none ^
 -prom-env input-device=/vdevice/vty@71000000 ^
 -prom-env output-device=/vdevice/vty@71000000 ^
 -prom-env use-nvramrc?=true ^
 -prom-env "nvramrc=dev /packages/disk-label h# 80000 to max-prep-partition-blocks device-end dev / s\" IBM,0210A73A1\" encode-string s\" system-id\" property device-end" ^
 -boot order=c ^
 -device spapr-vscsi,id=scsi0 ^
 -drive file=aix73.qcow2,if=none,id=hd0,format=qcow2 ^
 -device scsi-hd,drive=hd0,bus=scsi0.0,x-aix-vscsi-id=qemu-aix73-rootvg ^
 -drive if=none,id=cd0,media=cdrom,readonly=on ^
 -device scsi-cd,id=cdrom0,drive=cd0,bus=scsi0.0,scsi-id=1,lun=0 ^
 -nic user,model=spapr-vlan,hostfwd=tcp:127.0.0.1:7323-:23,hostfwd=tcp:127.0.0.1:7322-:22,hostfwd=tcp:127.0.0.1:7314-:514 ^
 %*
