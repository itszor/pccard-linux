The boot partition (apda1), FAT16, booted by FreeDOS:

  KERNEL.SYS    FreeDOS's kernel, from its FAT16 boot sector
  FDCONFIG.SYS  a menu: Linux on the text console (the default, after 3s),
                Linux on the VESA framebuffer, or a FreeDOS prompt
  LOADLIN.EXE   loads Linux from DOS; run as FreeDOS's shell
  LINUX.PAR     the kernel command line for each; netdev= tells the kernel
  LINUXFB.PAR   where !PC's NE2000 is (PCNE2: IRQ 5, 0x340), as left to itself
                it would probe 0x300, where !PC's HPC ports are
  BZIMAGE       the kernel
  COMMAND.COM   FreeCOM, for the FreeDOS prompt

SYSLINUX was tried first, from the MBR: it loads the kernel but will not
start it on !PC's BIOS ("Booting kernel failed: Invalid argument").
loadlin is what DSL has always booted with on !PC.
