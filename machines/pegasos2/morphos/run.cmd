@echo off
cd /d "%~dp0"
rem MorphOS 3.21 on a Genesi/bPlan Pegasos II (pegasos2): MPC7457 G4, 1 GB RAM. See run.sh.
bin\windows-x86_64\qemu-system-ppc.exe ^
 -name "MorphOS 3.21 G4 PEGASOS2" ^
 -M pegasos2 -m 1024 ^
 -kernel boot.img ^
 -vga none -device ati-vga,model=rv100,romfile=,bus=pci.0 ^
 -drive file=morphos.qcow2,if=ide,index=0,format=qcow2 ^
 -drive if=ide,index=2,media=cdrom,id=cd0 ^
 -nic user,model=via-rhine ^
 -monitor stdio ^
 %*
