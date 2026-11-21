# Sony MD DATA drive on the PC

`-device scsi-md` is a Sony MDH-10 MD DATA drive (`model=mdm110` for an
MDM-110). The disc, its U-TOC and ejecting are as on the Quadra 800: see
[the q800 notes](../q800/README.md#sony-md-data-drive).

Windows 95 drives it through the AMD PCscsi controller (Windows' AMSINT
driver):

    -device am53c974,id=scsi0 \
    -drive file=disc.img,if=none,id=md0,format=raw \
    -device scsi-md,bus=scsi0.0,scsi-id=3,drive=md0

- **No PCI bus?** If Device Manager has no "PCI bus" under System
  devices (the "Plug and Play BIOS" entry has a warning), Windows never
  sees the controller. Add it by hand: Add New Hardware, No, System
  devices, PCI bus, then restart. Windows then finds the AMD PCI SCSI
  Controller.
- **The drivers** are on Sony's two Windows 95 floppies. The INF that
  matches the drive is `US\MDH10.INF` on disk 1, and the VxDs it copies
  (`MDHLP.VXD`, `MDMGR.VXD`, `MDFSD.VXD`) are in the root of disk 1. The
  disc gets a drive letter ("Removable Disk"). `MDFMT.EXE` on disk 2
  formats discs.
- A disc formatted on the Mac reads and writes on Windows.
