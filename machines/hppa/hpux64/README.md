# 64-bit HP-UX 11i v1 and v2 on the HP C3700

**TL;DR** `-M C3700` runs 64-bit HP-UX 11i v1 (B.11.11) and 11i v2 (B.11.23) on one
PA-8700 CPU. Both install from their CDs, boot from disk to the console login, and run
Java. Seven QEMU changes and four firmware changes make this work. There is no kit yet.

## Run

[run.sh](run.sh) `11.11` or `11.23` starts LEMU's build (`build/qemu-system-hppa`, or
`$QEMU`) with the disk `hpux1111.qcow2` or `hpux1123.qcow2` on SCSI ID 6. The console is
the first serial port, in the terminal (`-nographic`). Ctrl-A c goes to the QEMU monitor
and back, Ctrl-A x quits QEMU. Halt HP-UX first with `reboot -h`.

The network is QEMU's user network. The install leaves the LAN card without an address,
and `/etc/hosts` gives the host name 127.0.0.1. Then DCE `rpcd` stops at boot ("Start
RPC daemon if needed ... FAIL"), and on 11.11 `samd`, which needs `rpcd`, respawns all
the time. Give the guest QEMU's address 10.0.2.15, change the host name's line in
`/etc/hosts` to 10.0.2.15, and reboot:

    ch_rc -a -p 'IP_ADDRESS[0]=10.0.2.15' -p 'SUBNET_MASK[0]=255.255.255.0' -p 'INTERFACE_STATE[0]=up' /etc/rc.config.d/netconf
    ch_rc -a -p 'ROUTE_DESTINATION[0]=default' -p 'ROUTE_GATEWAY[0]=10.0.2.2' -p 'ROUTE_COUNT[0]=1' /etc/rc.config.d/netconf

Then `telnet 127.0.0.1 11113` (11.11) or `11233` (11.23) reaches the guest.

## Install

- Make a disk (`qemu-img create -f qcow2 hpux1111.qcow2 20G`), put the install CD on
  SCSI ID 2 (`-drive if=scsi,bus=0,index=2,media=cdrom,readonly=on,format=raw,file=...`)
  and boot it with `-boot d`.
- Ignite runs on the serial console. At "No user interface", type 2 Enter, Enter, u, o.
  At the warning about data on the disk, type n. Change CDs in the QEMU monitor
  (`info block`, then `change`).
- The first boot from disk on 11.11 runs `hpux reflash.6750` once (a firmware tool), which
  resets the machine. The next boot runs `hpux`.
- In swconfig, the cimserver MOF build takes about 20 minutes. It makes many TLB misses;
  it does not hang.
- root has no password after the install.

## What changed in QEMU

- **SuperIO interrupts** (`hw/isa/pc87560.c`). The PIC in the NS87560 now knows edge and
  level interrupts (ELCR, config register 67h). A poll acknowledges an edge interrupt, and
  ICW1 resets the edge detector. Before, IRQ 6 came again and again in `superio_isr`.
- **SCSI SCRIPTS, three fixes** (`hw/scsi/lsi53c895a.c`, two of them as properties that
  the C3700 and A400 turn on):
  - A register-to-register move "SFBR to SFBR" (bit 23) used the wrong operand. The c8xx
    driver's idle loop then started an empty job for target 0.
  - `select-continue`: a second SELECT while the chip's own selection runs goes on, and a
    selection timeout stops SCRIPTS only at a phase compare. Before: "Already reselected"
    loops and "lsp is NULL on Selection Time-out".
  - `wait-complete`: SCRIPTS wait while a command without data (a tagged SYNCHRONIZE
    CACHE) is not complete. Before, the bus showed a false DATA IN phase, and the first
    boot from disk on 11.23 panicked with "lsp is NULL on phase mismatch".
- **PCI interrupt transactions** (`hw/pci-host/astro.c`). A PCI write to
  0xfee00000 + (id << 12) + (eid << 4) is an interrupt for that CPU. The c8xx SCRIPTS
  send one after each command. Before, the 11.23 kernel waited for ever.
- **Protection IDs** (`target/hppa`). On PA 2.0, a write is allowed when any PID that
  matches allows it, not only the first match. HP-UX 11.23 loaded the same PID twice, once
  write-disabled, and a copy to user space looped in the protection ID trap handler.
- **TCG deposit** (`tcg/tcg.c`, also on master). On AArch64 hosts both inputs of a deposit
  may be constant zero. When the optimizer did not fold it, the register allocator gave
  `out_rri` the constant as a register number, and the result took bits of host register
  x0. This broke the stubs that the HotSpot JVM writes, and `ServCtlMgr` on 11.11 hung.
  Now two constant inputs give one `movi`.
- Debug aid: the trace event `hppa_tlb_ar_fault` shows access-rights faults.

## Firmware

LEMU's SeaBIOS-hppa 27 with four changes, from github.com/tenox7/seabios-hppa branch
`hpux11` (offered upstream as hdeller/seabios-hppa pull request 9):

1. The console path names SuperIO serial port 1, not the SuperIO. Else HP-UX finds no
   console tty, and Ignite stops with "Cannot open /dev/console".
2. The IODC of the PCI host bridge (Elroy) does block I/O. Else HP-UX limits swap and
   dump to the first 4 GB, and lvlnboot stops with "Swap logical volume size beyond the
   IODC max address".
3. IODC disk I/O uses the drive in the layers, not always the boot drive.
4. PDC_RDR (144) gives PDC_BAD_PROC without a warning. 11.11 calls it again and again.

## Not working yet

- HP-UX 11i v3 (B.11.31): "ccio/sba driver not found". It needs PAT firmware.
- A400: PAT is not complete (`pat_init_cell` and others).
- No graphics console for 64-bit HP-UX: serial only.
- Crash dumps through IODC: ENTRY_IO on the Elroy returns BAD_OPTION.
- Only one CPU is tested.
