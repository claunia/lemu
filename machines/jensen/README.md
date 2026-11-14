# DEC Jensen (DECpc AXP 150, DEC 2000)

`-M jensen` is DEC's first Alpha PC: an EV4 CPU, EISA, the 82357
interrupt and DMA controller, an Adaptec AHA-1742 SCSI adapter, an
S3 86C928 video card with a Bt485 RAMDAC and, if you add it, a DEC DE422
network card. It runs the DEC V2.2 firmware
(ARC 4.31 and the SRM console), Windows NT 3.1, 3.51 and 4.0 for Alpha,
and OpenVMS AXP V1.5-1H1. Windows 2000 dropped the Jensen.

| Folder | What |
|---|---|
| `nt31/` | run.sh, run.cmd and README.txt of the nt31axp-lemu kit |
| `nt40/` | the same for the nt40axp-lemu kit |
| `release/` | build, check and pack the kits |

## Machine options

| Option | What it does |
|---|---|
| `s3-928=on` | the S3 86C928 card instead of plain VGA |
| `s3-vram-mb=4` | S3 video memory, 2 (default) or 4 MB; 1280x1024 with 65536 colours needs 4 |
| `s3-romfile=fw/928-n9.bin` | the card's video BIOS (Number Nine) |
| `rtc-ram-file=rtc.bin` | keeps the 66 bytes of battery-backed RAM in a file |
| `keyboard-locked=on` | the front panel key lock |
| `nt-idle=off` | let NT's idle loop spin; by default QEMU halts the CPU while NT is idle, so an idle VM uses about 2% of a host core |
| `board-id=off` | no system board EISA ID at port 0xc80; OpenVMS needs it off, else it reads slot 0's configuration and crashes (INVEXCEPTN) |

## Files

- `feprom0.bin`, `feprom1.bin`: the two 1 MB flash chips,
  `-drive if=pflash,index=0` and `index=1`. feprom0 is the V2.2 firmware
  (`fw/jensen-v2.2.bin`); the firmware keeps its settings in feprom1.
- `rtc.bin`: see `rtc-ram-file`.

## Notes

- The firmware rejects current years, so `run.sh` starts the clock 28
  years back.
- At every start the firmware warns that NVRAM boot selections are
  inconsistent. The warning is harmless.
- NT 3.1 keeps the screen blank from the boot until the desktop.
- NT 4.0 SP6a needs AMASK to copy Rb to Rc on the EV4 (commit 775c830bee,
  first made on the jensen branch). Without it Explorer dies with an
  illegal instruction.
- The jensen and es40 machines turn on four switches that keep other
  machines unchanged: `edge-irq-withdrawal` and `ltim-supported=off` on
  the 8259, `pf-without-pie` on the RTC and `selftest-reply-first` on the
  i8042.
- The RTC's periodic interrupt is the clock tick of NT (128 a second) and
  VMS (1024 a second). On macOS QEMU's main loop waits in whole
  milliseconds, and two ticks that come before the guest answers the first
  reach it as one: VMS's clock ran 24% slow. The jensen machine therefore
  uses the RTC's `slew` lost tick policy, which gives such ticks again, up
  to a quarter second of them. `-global mc146818rtc.lost_tick_policy=discard`
  turns it off.

## OpenVMS

OpenVMS AXP V1.5-1H1 boots from the SRM console (`b dka0`). It needs:

- `-M jensen,board-id=off`.
- On the AHA-1742 disk: `quirk_mode_page_rw_error_vms=on` and
  `quirk_mode_page_format_device=on`.
- On the CD (an RRD42): `physical_block_size=512`,
  `quirk_mode_page_set_block_size=on`, `quirk_mode_sense_rom_use_dbd=on`,
  `quirk_mode_page_rw_error_vms=on`, `quirk_mode_sense_rom_wp=on`,
  `scsi_version=2`, `vendor=DEC`, `product='RRD42   (C) DEC'`, `ver=4.5d`.
- The SRM console moves the RTC's year from the ARC count (from 1980) to the
  VMS one (from 1900) at power-on. The jensen machine starts the RTC in
  binary mode, as the firmware leaves it. In BCD the console read 1998 as
  2004, wrote year 4, and VMS asked for the date and time at every start.
- VMS's network card is the DE422 (`ERA0`, see below). With the card
  but without its EISA configuration, VMS hangs at boot.

## Network: DE422

`-device jensen-de422,netdev=n0` adds a DEC DE422-SA EtherWORKS EISA
card: an Am7990 LANCE with 64 KB of memory on the card. It sits in EISA
slot 5 (`slot=1` to `6`) and takes `mac=` like any NIC. The firmware's
power-up test checks it: `NI OK 52-54-00-12-34-56 DE422`. The card's
128 KB mode is not there; OpenVMS does not use it.

The EISA configuration must list the card before a system uses it. The
ECU floppy `ecu18nt.img` (ECU V1.8) has its file, `!DEC4220.CFG`:

1. Start with the floppy and without a boot disk, so the console stops
   at `>>>` (Ctrl-C does not stop the autoboot).
2. Type `runecu`. On a new feprom1 it opens the ARC setup menu first and
   wants the yellow items done in order: set the default environment
   variables, then the default configuration. Reset, then `runecu` again.
3. In the ECU, step 2 shows the card in slot 5. Keep its defaults
   (IRQ 5, memory D0000, 64 KB), then save and exit.

The configuration is kept in feprom1.
