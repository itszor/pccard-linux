# Linux on the RiscPC PC card: research notes

Notes, 2026-10-26, on what it would take to run Linux on the Aleph One / Acorn
x86 second-processor cards for the RiscPC (486- and 5x86-class "Gemini" cards),
running under !PC / PCPro.

These are kept as they were written, before any of this project existed, so
some of it was overtaken: the disc driver is option A below (`apcdisk`, in
`drivers/`), and the kernel is 4.19, not 2.x.

## Sources

- **GPL source release (2005):** <https://riscos.info/pc/>
  - `files/Src306.zip`: !PC 3.06 source (ARM side, BIOS, PC-side drivers)
  - `files/Driver-src.zip`: PC-side driver sources (Windows, CD-ROM, network, sound, mouse...)
  - `files/DDKs.zip`: Device Driver Kits, including the HPC spec
  - `files/Rel306.zip`, `files/Rel307.zip`: binaries (3.06 release, unreleased 3.07)
  - `files/Src234.zip` (2.34 source) was **404** when checked
  - The release page says all Aleph One copyright code is now GPL, and Stuart
    Tyrell's code is too. That supersedes the 1997 "may not be distributed"
    notice still in `DDK.txt`.
- PCPro 3 advert: <https://riscos.info/pc/AcornProd/Software/PCPro3Demo/index.html>
- PCPro 3 User Guide: <https://riscos.info/pc/AcornProd/docs/manuals/PCPro3/index.html>
  (single-page version: `Images/PCP3Man.html`)
- ARMEdit's own HPC documentation was not found online. It isn't needed: the
  Aleph One DDK is the authoritative spec, and it lists ARMEdit as HPC service `0105h`.

Note: riscos.info returns 403 to some fetchers. A normal browser User-Agent works.

Paths in these notes are relative to a directory holding those three zips
(`DDKs.zip`, `Driver-src.zip`, `Src306.zip`) unpacked, as `DDKs/`,
`Driver-src/` and `Src306/`. They aren't in this repository.

## How the card works

The x86 CPU runs natively against RAM. The glue logic (the "Gemini" ASIC) traps
I/O port accesses and some memory ranges, stalls the x86, and the ARM-side !PC
emulates the device. The manual describes three levels of emulation:

| Level | Used for |
|---|---|
| I/O level (trapped port accesses) | VGA/CGA, keyboard, bus mouse, NE2000, serial, parallel, RTC, PIC/PIT |
| BIOS level (INT 13h over HPC) | Hard discs, floppies |
| Driver level (HPC packets) | CD-ROM (`HPC_CD.SYS`, `CDHPC.MPD`), SCSI/ASPI, Windows display (ARMDRV/ARMDX) |

Other limits:
- PC RAM can only be set to preset sizes: 1–8, 10, 12, 14, 16, 20, 24, 28 or 32 MB.
  "Include VRAM" subtracts the VRAM size from this.
- Gemini I has a 128K cache; Gemini II has 128K or 512K.

## The blocker: no disk emulation at the I/O level

From the PCPro 3 manual:

> Hard and floppy drives are not emulated at this level [I/O], due to speed,
> but this is to be addressed in future releases of PCPro 3, since it stops
> non DOS-based operating systems from even installing.

So Linux's IDE and floppy drivers will find nothing. Discs exist only through
the BIOS (INT 13h → HPC), and only two BIOS drives are supported.

## The HPC (High-level Procedure Call) protocol

Spec: `DDKs/PCddkv300/Docs/DDK.txt`, section 5 and **Appendix A, "Low-level PC
interface to HPC"**, which is written for "those developing HPC drivers for other
operating systems". It's plain port I/O and doesn't need the BIOS.

### Ports

| Port | Dir | Use |
|---|---|---|
| `0x300` | W (byte) | Command port |
| `0x300` | R (byte) | Status port |
| `0x304` | R/W (byte/word) | Data, buffer A |
| `0x306` | R/W (byte/word) | Data, buffer B |
| IRQ 15 | | Completion / suspend interrupt |

The source (`Src306/PC/CPU/c/HPC`) also defines `0x302` (req) and debug ports at
`0x300`/`0x301`/`0x307`.

Optional shared memory: a 32K window somewhere in `0xC0000–0xEFFFF`, used as a
faster transfer area. The I/O-port path is enough to start with.

### Command values (`b` = buffer: 0 = A, 1 = B; `i` = interrupt on completion)

```
0000 000b  HPC_WRITE_IO
0001 000b  HPC_READ_IO
0010 000b  HPC_WRITE_MEM
0011 000b  HPC_READ_MEM
010i 000b  HPC_DO_CALL
0110 000b  HPC_RELEASE_BUFFER
1000 0000  HPC_RESET_ACK
1000 0010  HPC_SUSPEND_ACK
1000 0100  HPC_MULTI_REQ
1000 0110  HPC_QUIT_REQ
1000 1000  HPC_FREEZE_REQ
```

### Status bits

- bit 7: suspend requested
- bit 5: ready / command complete (0 = busy)
- bit 1: buffer B in use
- bit 0: buffer A in use

### Call sequence

1. Read the status port and wait until the chosen buffer is free.
2. Write `HPC_WRITE_IO` (or `_MEM`). The ARM marks the buffer in use.
3. Send the packet: `REP OUTSW` to the data port, or write it into shared memory.
4. Write `HPC_DO_CALL`, with the `i` bit set if you want IRQ 15 on completion.
5. Poll or wait until status bit 5 is set.
6. Write `HPC_READ_IO` (or `_MEM`) and read back the result (`REP INSW`).
7. Write `HPC_RELEASE_BUFFER`. Always do this, even on an error path.

Buffer A is for the swap/disk driver (kernel mode, never pre-empted by itself).
Buffer B is for everything else and needs locking. The ARM does essentially no
PC emulation while a call is in progress, so polling is fine.

Suspend mode (Appendix A.3): when the ARM sets status bit 7 and raises IRQ 15,
the PC should mask all other IRQs, write `HPC_SUSPEND_ACK`, and `HLT` until it
gets an interrupt. The manual says this mattered for the old podule cards' DRAM
refresh; a Linux driver should still handle it.

### Packets

- PC → ARM: `u16 service_id` followed by service-specific data, up to 16K. There's
  no length field, so the service works the length out from its parameters.
- ARM → PC: up to 16K. An unknown service returns `FF FF FF FF`.

Service IDs:

| ID | Service |
|---|---|
| `0000h` | System information |
| `0001h` | Hard disk |
| `0002h` | Floppy disk |
| `0003h` | CD-ROM |
| `000Fh` | Software development (for testing) |
| `0105h` | ARMEdit (Alexander Thoukydides) |
| `0106h` | WinRisc (Chris Claydon) |
| `0107h` | ASPI (Gary Partis, Powertec) |
| `0108h` | ASPI (Andreas Walter, freeware) |

Under DOS the BIOS wraps all of this in `INT 4Dh` (AX=0 identify, which returns
`4850h` "HP"; AX=1 call with a 26-byte scatter/gather parameter block at ES:BX).
Linux would drive the ports directly.

### Hard disk service (`0001h`)

`Src306/PC/DEV/h/HPC_HD` (structures), `Src306/PC/DEV/c/HDD_HPC` (ARM implementation):

- functions: `INFO` (0), `READ` (1), `WRITE` (2), `MEDIA` (3)
- 512-byte sectors, up to 4 drives, max transfer `16384-16` bytes
- drive flags: read-only, removable, lockable, volatile
- errors: OK, unknown, bad param, bad addr, data error, write-protect, media changed

Before writing a driver, check the exact packet layout against
`HPC_HD`/`HDD_HPC` and the BIOS's INT 13h handler.

## Unfinished IDE emulation in the source

`Src306/PC/DEV/c/IDEEm` (783 lines, started 1998-02-08, header
"Emulation of IDE hard drive / CD-ROM controller") emulates an IDE controller at
register level on top of the HPC disk backend. `DEV/c/FddEm` does the same for a
floppy controller. Both are compiled and linked (`o.ideem`, `o.fddem` in the
MakeFile), but their init calls are commented out in `Src306/PC/SYS/c/Main`
(around line 1388):

```c
        !FDD_Init()  ||
        //!FDDEm_Init()||
        //!IDEEm_Init()||
        !HDD_Init()  ||
```

This is presumably the "future release" the manual promised. Finishing it would
let stock Linux IDE drivers work without guest-side changes, but `!PC` would have
to be rebuilt.

## Graphics

- PCPro 3 has **VBE 2.0 with a linear framebuffer**. With "include VRAM" ticked,
  the RiscPC's real VRAM is mapped into PC memory, so writes are full speed with
  no trapping. That suits `vesafb` (`vga=0x...` / `vga=ask`).
- A VBE 2.0 mode forces !PC into single-tasking (full-screen) mode. That's fine for Linux.
- High-colour modes are listed as "32K", which is probably RISC OS's native 15bpp
  layout, possibly BGR order. `vesafb` and X read the VBE mode masks, so check
  that the BIOS reports them correctly. The code is in `Src306/PC/VID`, and the
  BIOS is in `Src306/PC/PCside/BIOS/Bios.zip`.
- VBE 1.2 banked modes are available too. Xvesa (TinyX/kdrive) or the XFree86 4.x
  `vesa` driver would cope with either.
- The video memory dynamic area is 256K–2MB.

## Other devices

- **NE2000** emulation (needs the separate Network Links package; module `PCNE2`, IRQ
  configurable). Linux's stock `ne` driver should work, and makes NFS root possible.
- **Bus mouse** on IRQ 3 (AMOUSE.COM). It isn't clear yet whether it emulates the
  Logitech or the InPort type; check `DEV`/`amouse` source.
- **Serial and parallel** are emulated at I/O level. Serial is a good kernel console.
- **Sound Blaster**: a minimal emulation at port 220, IRQ 7, DMA 1.
- **Keyboard** (8042), **RTC**, and **PIC/PIT** are emulated at I/O level. PIT
  accuracy is still unknown and is the first thing to check, because Linux
  calibrates its delay loop against it.
- **VGA retrace** emulation is approximate and configurable.

## Kernel choice

- The early 486SLC/SXL-class cards (if that's what they are) need a kernel no
  newer than 3.7, because 386-class support was removed in 3.8. i486 support is
  also being removed from mainline.
- With at most 32MB and slow trapped I/O, **2.2, 2.4 or early 2.6** is the practical
  choice. Use HZ=100.

## Suggested plan

1. **Test without a disk driver.** Boot with LOADLIN from DOS on the BIOS drive,
   using an initrd root, or an NFS root over the NE2000 emulation. Use a serial
   console and `vga=` with `vesafb`. Likely hang points: delay-loop/TSC
   calibration (try `lpj=` / `notsc`), the i8042 probe (`i8042.noaux` etc.), and
   memory sizing (`mem=`).
2. **Read the timer/PIC emulation** in `Src306/PC` before chasing hangs.
3. **Disk, option A (guest side):** write a Linux `hpc-blk` block driver (ports
   `0x300–0x306`, buffer A, IRQ 15 or polling, service `0001h`). It's probably a few
   hundred lines and needs no host changes.
4. **Disk, option B (host side):** finish and enable `IDEEm`/`FddEm` in !PC and
   rebuild. Stock drivers then work, and it would help any non-DOS OS.
5. A last-resort hack: a block driver that calls the BIOS INT 13h in vm86 mode.

## Useful files in the source tree

| Path | Contents |
|---|---|
| `DDKs/PCddkv300/Docs/DDK.txt` | DDK spec: device modules, PCSupport/PCDevHelp SWIs, HPC |
| `DDKs/PCddkv300/Code/` | Example device module (PCDevHelp) |
| `DDKs/ARMDDK/docs/GDISpec` | Windows display driver (ARMDRV) GDI interface |
| `Src306/PC/CPU/c/HPC` | ARM side of the HPC port interface |
| `Src306/PC/DEV/c/HDD_HPC`, `DEV/h/HPC_HD` | Hard disk HPC service |
| `Src306/PC/DEV/c/Fdd_hpc`, `DEV/h/HPC_FD` | Floppy HPC service |
| `Src306/PC/DEV/c/CD_HPC`, `DEV/h/HPC_CD` | CD-ROM HPC service |
| `Src306/PC/DEV/c/IDEEm`, `DEV/c/FddEm` | Unfinished I/O-level IDE/FDC emulation |
| `Src306/PC/DEV/c/NE2000`, `Kbd`, `RTC`, `MultiIO` | Other device emulation |
| `Src306/PC/VID/` | Video emulation, VBE |
| `Src306/PC/PCside/BIOS/Bios.zip` | PC BIOS source (INT 13h, INT 10h/VBE, INT 4Dh HPC) |
| `Src306/PC/PCside/DOSCD/HPC_CD.ASM` | DOS CD-ROM driver: a reference HPC client |
| `Src306/PC/PCside/{ARMDRV,armdx,amouse,Pcsleep}` | PC-side drivers |
| `Src306/PC/SYS/c/Main` | Device init order (the commented-out IDEEm/FddEm) |
