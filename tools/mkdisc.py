#!/usr/bin/env python3
"""Make the PC card's hard disc image, for !PC's HD0.

    mkdisc.py SIZE_MB BZIMAGE ROOTFS DOSBOOT BOOTFILES OUT

!PC serves a disc image file as a disc of 17-sector tracks, with as many
heads as keep it within 1024 cylinders (DEV.c.HDD_HPC, File_setconfig).
The image gets the same geometry in its partition table:

  MBR        SYSLINUX's, which boots the active partition
  apda1      FAT16, from sector 17 (the first track is the MBR's, as DOS
             has it), with FreeDOS's boot sector installed as its SYS
             would: FreeDOS, FreeCOM, loadlin, the kernel, and BOOTFILES
             (FDCONFIG.SYS and loadlin's .PAR files)
  apda2      ext2, the root filesystem, to the last whole cylinder

DOSBOOT holds kernel.sys, command.com, loadlin.exe and fat16.bin, the
assembled FAT16 boot sector.

Run as root, so that the root filesystem keeps its owners.
"""
import os, struct, subprocess, sys, tempfile

SPT = 17
BOOT_MB = 8
MBR_CODE = '/usr/lib/syslinux/mbr/mbr.bin'


def run(*cmd):
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)


def geometry(nsects):
    """!PC's: heads 4, 6, 8... until there are no more than 1024 cylinders."""
    heads = 4
    while heads < 0xFE and nsects // (SPT * heads) > 1024:
        heads += 2
    return heads


def chs(lba, heads):
    c, r = divmod(lba, heads * SPT)
    h, s = divmod(r, SPT)
    c = min(c, 1023)
    return bytes([h, ((c >> 2) & 0xC0) | (s + 1), c & 0xFF])


def entry(boot, ptype, start, count, heads):
    return (bytes([0x80 if boot else 0]) + chs(start, heads) + bytes([ptype]) +
            chs(start + count - 1, heads) + struct.pack('<II', start, count))


def install_freedos(fat, bootsect, heads):
    """FreeDOS's FAT16 boot sector, as its SYS installs it (sys/sys.c,
    put_boot): the code from fat16com, the disc's parameters (bytes
    0x0B-0x3D) from the formatted boot sector, and the boot drive."""
    new = bytearray(open(bootsect, 'rb').read())
    assert len(new) == 512 and new[510:] == b'\x55\xaa'
    # The places SYS patches, checked as it checks them.
    assert new[0x178:0x17A] == b'\x84\xd2', 'LBA test not at 0x178'
    assert new[0x5C:0x5E] == b'\x60\x00', 'load segment not at 0x5C'
    assert new[0x66:0x68] == b'\x88\x56', 'drive store not at 0x66'
    with open(fat, 'r+b') as f:
        old = f.read(512)
        new[0x0B:0x3E] = old[0x0B:0x3E]
        new[0x03:0x0B] = b'FRDOS5.1'
        struct.pack_into('<HH', new, 0x18, SPT, heads)   # the disc's geometry
        new[0x24] = 0x80                                 # the first hard disc
        f.seek(0)
        f.write(new)


def main():
    size_mb, kernel, rootfs, dosboot, bootfiles, out = sys.argv[1:7]
    nsects = int(size_mb) * 2048
    heads = geometry(nsects)
    cyl = heads * SPT
    nsects = nsects // cyl * cyl                  # whole cylinders only

    boot_start = SPT
    boot_end = -(-(boot_start + BOOT_MB * 2048) // cyl) * cyl
    root_start, root_end = boot_end, nsects
    boot_n, root_n = boot_end - boot_start, root_end - root_start

    with tempfile.TemporaryDirectory() as tmp:
        fat = os.path.join(tmp, 'boot.img')
        run('mkfs.fat', '-C', '--invariant', '-F', '16', '-s', '2',
            '-n', 'PCBOOT', '-h', str(boot_start), fat, str(boot_n // 2))
        install_freedos(fat, os.path.join(dosboot, 'fat16.bin'), heads)
        for name in ('kernel.sys', 'command.com', 'loadlin.exe'):
            run('mcopy', '-i', fat, os.path.join(dosboot, name),
                '::' + name.upper())
        run('mcopy', '-i', fat, kernel, '::BZIMAGE')
        for name in sorted(os.listdir(bootfiles)):
            run('mcopy', '-i', fat, os.path.join(bootfiles, name),
                '::' + name.upper())

        ext = os.path.join(tmp, 'root.img')
        run('mke2fs', '-q', '-t', 'ext2', '-L', 'root', '-m', '1',
            '-d', rootfs, ext, f'{root_n // 2}k')

        mbr = bytearray(open(MBR_CODE, 'rb').read()[:440].ljust(446, b'\0'))
        mbr += entry(True, 0x06, boot_start, boot_n, heads)
        mbr += entry(False, 0x83, root_start, root_n, heads)
        mbr += bytes(32) + b'\x55\xaa'

        with open(out, 'wb') as f:
            f.truncate(nsects * 512)
            f.write(mbr)
            for path, start in ((fat, boot_start), (ext, root_start)):
                f.seek(start * 512)
                with open(path, 'rb') as part:
                    f.write(part.read())

    print(f'{out}: {nsects * 512 >> 20}MB, {heads} heads, {nsects // cyl} '
          f'cylinders; apda1 {boot_n // 2048}MB FAT, apda2 {root_n // 2048}MB ext2')


if __name__ == '__main__':
    main()
