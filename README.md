# Linux for Aleph One's PC card

A Debian system for the x86 PC card in an Acorn Risc PC, run by !PC. It is
built here, on any x86 Linux machine with Docker, into one disc image that
!PC serves as the PC's HD0:

    ./build.sh

and `out/disc.img` is the disc. Copy it to RISC OS as a DOSDisc file (type
`FC8`, e.g. `Linux,fc8` on a HostFS or ShareFS share), and give it to !PC
as HD0 in !PCConfig's Disc setup.

## What is on it

| | |
|---|---|
| `apda1` | 8MB FAT16, booted by FreeDOS: a menu (`FDCONFIG.SYS`) runs `loadlin` with the kernel for the desktop (the default; the VESA framebuffer at 800x600) or the text console, or gives a FreeDOS prompt |
| `apda2` | ext2, the root filesystem: Debian 7 (wheezy), minimal |
| `apdc` | swap, if !PC has some to give: !PC 3.09's `SwapSize` option keeps it in a RISC OS dynamic area. It is made afresh at each boot. |

- **The kernel** is Linux 4.19, configured for this machine only: a 486, ISA,
  !PC's emulated keyboard and NE2000, VESA framebuffer. It has no initrd and
  no modules it needs to load.
- **apcdisk** (`drivers/`), built in, gives it !PC's discs through HPC, !PC's
  calls for its PC-side drivers.
- **Debian 7** is the last Debian built for the i486. Every program and
  library is checked for instructions a 486 lacks (`./build.sh check486`),
  except a few libraries that choose their code by CPU at run time, named
  with the reason in `rootfs/check486.allow`.
  Its packages come from `archive.debian.org`, checked against Debian's
  retired archive keys.
- **The network** is DHCP on !PC's NE2000 (PCNE2: IRQ 5, port 0x340), which
  PCNAT answers.
- **The desktop:** user `pc` is logged in on tty1, and starts X when booted
  with the framebuffer: Xorg's fbdev driver, JWM in RISC OS's colours, and
  ROX-Filer's pinboard with a panel along the bottom, like the icon bar.
  The panel has Terminal (xterm), Dillo, a small web browser (HTTP only:
  wheezy's Dillo has no HTTPS), and Edit. Ctrl+Alt+T opens a terminal;
  Ctrl+Alt+Backspace leaves the desktop. With an xterm open it uses about
  10MB of the PC's 27MB.
- **Passwords:** `root` and `pc` have none, for the console. sshd is
  running, but it refuses empty passwords, so set one (`passwd`) to log in
  over the network. Its host keys are made at first boot, as the image is
  shared.
- **ROX's applications**, in `/usr/local/apps` (the panel's Apps):
  - Edit, Archive, Memo, MIME-Editor and Wallpaper, in Python, on
    ROX-Lib2 (`/usr/local/lib`) and wheezy's pygtk. Each is about 10MB of
    Python and GTK, so running them pushes the rest of the desktop out to
    swap.
  - Pager, a panel applet (drag it onto the panel), and OroboROX, ROX's
    window manager, which iconifies windows onto the pinboard as RISC OS
    does: built from source for the i486 against wheezy (`./build.sh rox`),
    with ROX-CLib. To use OroboROX instead of JWM, see `~/.xinitrc`.
  - Not included: ROX-Session (about 10MB more for every session, where
    JWM and `.xinitrc` do its job), and the configlets that only configure
    it, or need X features the framebuffer driver lacks.
- **HTTPS:** today's root certificates (Mozilla's, as curl publishes them)
  replace Debian 7's, and curl (OpenSSL) can reach current sites. Debian
  7's wget (GnuTLS) mostly can't.
- **Tools:** less, ping, nano, wget, curl, ssh, ps and the rest; apt works, with
  `--allow-unauthenticated` (Debian 7's archive keys have expired; see
  `/etc/apt/apt.conf.d/10archive`).

## The CPU

The Cyrix 5x86 that Aleph One's cards used is a 486 to Linux, with no TSC,
CMPXCHG8B or CMOV. That rules out Debian 8 and later (i586 and i686), and
anything built on them, such as antiX. In RPCEmu (rpcemu-extended), test on
the `pc-486dx4` card, which lacks the same instructions; `pc-pentium-mmx`
has two of them.

## Building

`./build.sh` runs each step; `./build.sh STEP...` runs just those:

| Step | Makes |
|---|---|
| `fetch` | `downloads/`: the pinned sources, each checked against its SHA-256 |
| `container` | the Docker image the rest is done in (Debian 10, of Linux 4.19's era) |
| `kernel` | `out/bzImage`, from `config/pccard.config` over `tinyconfig`, with `drivers/` built in |
| `rootfs` | `out/rootfs`, with debootstrap and `rootfs/packages`, configured from `rootfs/`; then `check486`. The archive's Release file is checked against Debian's (expired) keys with gpgv, and its Packages file against that, as Debian 7's apt can't (`tools/wheezy.sh`). |
| `rox` | ROX's applications, into `out/rootfs`: the C ones built in a second Debian 7 chroot, `out/buildroot`, with GTK's headers (`tools/build-rox.sh`); the Python ones as released; then `check486` |
| `dosboot` | `out/dosboot`: FreeDOS's kernel and FAT16 boot sector (assembled from its source), FreeCOM, loadlin |
| `disc` | `out/disc.img`, by `tools/mkdisc.py`; `DISC_MB` sets its size (400) |

The `rootfs` and `disc` steps run their container privileged, for
debootstrap's chroot and to keep the root filesystem's owners. Nothing
outside this directory is changed except Docker's image, `pccard-kbuild`.
`downloads/`, `src/` and `out/` can be deleted to start again.

## Licences

Everything on the disc may be passed on:

- Linux, apcdisk, loadlin, FreeDOS, FreeCOM and ROX's applications are
  GPL. `./build.sh fetch` puts their source in `downloads/`: Linux's
  tarball, FreeDOS's and FreeCOM's source releases, Debian's loadlin
  source, and ROX's releases. apcdisk is in `drivers/`.
- This directory's own files (the scripts, configuration and apcdisk) are
  GPL-2.0, in `COPYING`.
- Debian's packages are free software, under their own licences, each in
  `/usr/share/doc/*/copyright` on the disc. Their sources are on
  `archive.debian.org`.
- !PC itself is not part of this. Aleph One released the source of !PC 3.06
  as open source.

MS-DOS 6 is not freely distributable, which is why the disc boots FreeDOS.
Microsoft has released only MS-DOS 1.25, 2.0 and 4.0 as open source.
