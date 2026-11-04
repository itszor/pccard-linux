#!/bin/sh
# Build Linux for Aleph One's x86 PC card under !PC, from nothing but this
# directory, an internet connection and Docker.
#
#   ./build.sh [step...]
#
# Steps, run in this order when none are named:
#   fetch      download the pinned sources into downloads/ and check them
#   container  build the Docker image the kernel is compiled in
#   kernel     configure and build the kernel, with our drivers (drivers/)
#              built in: out/bzImage
#   rootfs     a Debian 7 (wheezy) system for the i486, with debootstrap:
#              out/rootfs, configured from rootfs/
#   rox        ROX's applications, into out/rootfs: the C ones built in a
#              Debian 7 chroot (out/buildroot), the Python ones as released
#   check486   check that everything in out/rootfs runs on a 486 (part of
#              rootfs and rox, and can be run on its own)
#   dosboot    FreeDOS's kernel and boot sector, FreeCOM and loadlin, which
#              boot the disc: out/dosboot
#   disc       the disc image for !PC's HD0: out/disc.img (DISC_MB=400 by
#              default)
#
# Everything made goes in downloads/, src/ and out/, which can be deleted
# to start again. Nothing outside this directory is changed, except for
# the Docker image (pccard-kbuild).
set -eu

TOP=$(cd "$(dirname "$0")" && pwd)
cd "$TOP"

# Pinned sources: version, URL and SHA-256.
KVER=4.19.325
KURL=https://cdn.kernel.org/pub/linux/kernel/v4.x/linux-$KVER.tar.xz
KSHA=607bed7de5cda31a443df4c8a78dbe5e8a9ad31afde2a4d28fe99ab4730e8de1

# Debian 7 (wheezy): the last Debian built for the i486, from its archive.
SUITE=wheezy
MIRROR=http://archive.debian.org/debian
# Its keys are among those Debian has since retired.
KEYRING=/usr/share/keyrings/debian-archive-removed-keys.gpg
# Beyond the minimal base: networking (busybox for udhcpc).
PACKAGES=ifupdown,netbase,busybox,procps,kmod,apt-utils

# The disc image's size, in MB.
DISC_MB=${DISC_MB:-400}

# The boot partition: FreeDOS (kernel, and its source for the boot sector),
# FreeCOM, and Debian's loadlin, with their sources. All GPL. And today's
# root certificates (Mozilla's, as curl publishes them): Debian 7's are too
# old for most HTTPS sites now.
# ROX's applications, from the ROX project's SourceForge downloads (GPL).
ROX=https://downloads.sourceforge.net/project/rox
FDK=https://github.com/FDOS/kernel/releases/download/ke2046
FDCOM=https://github.com/FDOS/freecom/releases/download/com086
DOWNLOADS="
$FDK/ke2046_86f16.zip ke2046_86f16.zip 3fc4adb1f693526089df3d46a6512f2c2201d6cffdda40eac4b99d666dcd356a
$FDK/ke2046s.zip ke2046s.zip 0ea71c9276aba7e1e371f42e8e17d4e9223ac4bedeacccce3ee700fc2866c5e3
$FDCOM/English.zip freecom086-English.zip 42093a00286f66bf922eebd8204a3b74b429f4c6973cd960ae1ab52ce80b4bd5
$FDCOM/freecom-source.zip freecom086-source.zip 4d9365772efc54fe5bd4bc48a27bcbb7041e71791af36dd0621ef4c8a34cc1d3
$MIRROR/pool/main/l/loadlin/loadlin_1.6f-6_amd64.deb loadlin_1.6f-6_amd64.deb 24bc7ec9c07d52080e0d19d3ec9e77441e9d52314bc98185e123eee51a59fd25
$MIRROR/pool/main/l/loadlin/loadlin_1.6f.orig.tar.gz loadlin_1.6f.orig.tar.gz eb77f58a24e3e545c172cb5f75a89bd8ea3392826aca582de080bb173d05a5bf
$ROX/ROX-Lib/2.0.6/rox-lib2-2.0.6.tar.bz2 rox-lib2-2.0.6.tar.bz2 d0ddf57eeb43ef6628fd2045ba331f3a4550e385e2b84d430c8ebb517ed8ad2f
$ROX/Edit/2.2/edit-2.2.tar.bz2 edit-2.2.tar.bz2 c449003cae5a1fa240930642ac889fa3b9f9a0280bacd59cd0b6f86c60e77f02
$ROX/Archive/2.3/archive-2.3.tar.bz2 archive-2.3.tar.bz2 dc72728203874e0d4c07a079298cc628cdd30fd76a057f426ab7ad06f80ce7eb
$ROX/Memo/2.1/memo-2.1.tar.bz2 memo-2.1.tar.bz2 78704e7befd32b60944a48be5cecaccb4196cf87478777c71b64c47a521cc0e1
$ROX/MIME-Editor/0.6/mime-editor-0.6.tar.bz2 mime-editor-0.6.tar.bz2 de0708f824feea875c606d2c000749da542f117bf7922d8f1679f5aef9d9bb39
$ROX/Wallpaper/2.1/wallpaper-2.1.tar.bz2 wallpaper-2.1.tar.bz2 f990e85229e0bfd801bc005374c419ec7d294faba15f20af94325fd7d84fd7f7
$ROX/ROX-CLib/2.1.10/ROX-CLib-2.1.10.tar.gz ROX-CLib-2.1.10.tar.gz c8587e9c768df59a626cd1bdb4d6b2e132647f80036de23f8b6516253d725378
$ROX/Pager/1.2/Pager-1.2.tar.gz Pager-1.2.tar.gz 2440f2ffdf27036f3f630a8ca328b41ea265d05faa5dfc64a3cb6952bd4cc095
$ROX/OroboROX/0.9.8/OroboROX-0.9.8.1.tar.bz2 OroboROX-0.9.8.1.tar.bz2 d7d7a65fd1aa900b7ad5fefb43c6ad7cb17b5d7b07bc7df8962acf32ac35568d
https://curl.se/ca/cacert-2026-10-20.pem cacert-2026-10-20.pem 796f1389bfea13c0ed78f663b765baaff54a508e5483786bd6ece73c5e3e3cbe
$MIRROR/pool/main/l/loadlin/loadlin_1.6f-6.debian.tar.xz loadlin_1.6f-6.debian.tar.xz 3598f5d9fc9a79c66d3dc9c1f9ace739526ab39a349add4cfbb402eb8b233101
"

IMAGE=pccard-kbuild
KSRC=src/linux-$KVER
KOUT=out/kernel

say() { printf '\n== %s\n' "$*"; }

# fetch URL SHA256 [NAME]: into downloads/, checked
fetch_one() {
	f=downloads/${3:-$(basename "$1")}
	if [ ! -f "$f" ]; then
		curl -fL --retry 3 -o "$f.part" "$1"
		mv "$f.part" "$f"
	fi
	echo "$2  $f" | sha256sum -c --quiet - || {
		echo "$f: checksum mismatch; delete it and run again" >&2
		exit 1
	}
}

step_fetch() {
	say "fetch"
	mkdir -p downloads src
	fetch_one "$KURL" "$KSHA"
	echo "$DOWNLOADS" | while read -r url name sha; do
		if [ -n "$url" ]; then fetch_one "$url" "$sha" "$name"; fi
	done
	[ -d "$KSRC" ] || tar -C src -xf downloads/linux-$KVER.tar.xz
}

step_container() {
	say "container"
	docker build -q -t "$IMAGE" docker
}

# Run a command in the build container, in this directory, as us.
in_container() {
	docker run --rm -u "$(id -u):$(id -g)" -v "$TOP:/work" -w /work \
		-e KBUILD_BUILD_USER=pccard -e KBUILD_BUILD_HOST=pccard-linux \
		"$IMAGE" sh -c "$*"
}

# Our drivers, as drivers/alephpc in the kernel tree: a link to drivers/,
# and a line each in the kernel's own drivers/Makefile and drivers/Kconfig.
add_drivers() {
	ln -sfn ../../../drivers "$KSRC/drivers/alephpc"
	grep -q alephpc/ "$KSRC/drivers/Makefile" ||
		echo 'obj-y += alephpc/' >> "$KSRC/drivers/Makefile"
	grep -q alephpc/Kconfig "$KSRC/drivers/Kconfig" ||
		sed -i '$ i source "drivers/alephpc/Kconfig"' "$KSRC/drivers/Kconfig"
}

# Run a command in the build container as root, able to chroot and mount:
# for debootstrap, and for anything that must own files as root.
as_root() {
	docker run --rm --privileged -v "$TOP:/work" -w /work \
		"$IMAGE" sh -c "$*"
}

# The wheezy_* functions (tools/wheezy.sh), for as_root's scripts.
WHEEZY="SUITE=$SUITE MIRROR=$MIRROR KEYRING=$KEYRING; . tools/wheezy.sh"

step_rootfs() {
	say "rootfs: Debian $SUITE"
	as_root "
		set -e
		$WHEEZY
		wheezy_bootstrap out/rootfs $PACKAGES
		# The overlay first, so that dpkg's excludes apply to what is
		# installed after.
		cp -r --preserve=mode rootfs/overlay/. out/rootfs/
		cp rootfs/setup.sh downloads/cacert-2026-10-20.pem out/rootfs/tmp/
		wheezy_enter out/rootfs
		wheezy_install out/rootfs \$(sed 's/#.*//' rootfs/packages) &&
			chroot out/rootfs /tmp/setup.sh ||
			{ wheezy_leave out/rootfs; exit 1; }
		wheezy_leave out/rootfs
		rm out/rootfs/tmp/setup.sh out/rootfs/tmp/cacert-*.pem
		cp rootfs/overlay/etc/resolv.conf out/rootfs/etc/resolv.conf
	"
	step_check486
}

# ROX's applications, into out/rootfs: the C ones (ROX-CLib, Pager,
# OroboROX) built in a Debian 7 chroot with GTK's headers (out/buildroot),
# and the Python ones (ROX-Lib2 and the apps on it) as they are released.
# AppDirs in /usr/local/apps, libraries in /usr/local/lib, where ROX looks
# for them. Their sources stay in downloads/.
ROX_PYTHON_LIBS="rox-lib2-2.0.6.tar.bz2"
ROX_PYTHON_APPS="edit-2.2.tar.bz2 archive-2.3.tar.bz2 memo-2.1.tar.bz2 mime-editor-0.6.tar.bz2 wallpaper-2.1.tar.bz2"
ROX_BUILD_DEPS="build-essential autoconf automake pkg-config libgtk2.0-dev libxml2-dev libwnck-dev libxft-dev libxpm-dev"
step_rox() {
	say "rox"
	as_root "
		set -e
		$WHEEZY
		wheezy_bootstrap out/buildroot
		wheezy_enter out/buildroot
		wheezy_install out/buildroot $ROX_BUILD_DEPS ||
			{ wheezy_leave out/buildroot; exit 1; }
		mkdir -p out/buildroot/build/src
		cp downloads/ROX-CLib-2.1.10.tar.gz downloads/Pager-1.2.tar.gz \
			downloads/OroboROX-0.9.8.1.tar.bz2 tools/build-rox.sh \
			out/buildroot/build/src/
		linux32 chroot out/buildroot /build/src/build-rox.sh ||
			{ wheezy_leave out/buildroot; exit 1; }
		wheezy_leave out/buildroot

		# Built: everything but the sources and objects.
		L=out/rootfs/usr/local/lib A=out/rootfs/usr/local/apps
		mkdir -p \$L \$A
		rm -rf \$L/ROX-CLib \$A/Pager \$A/OroboROX
		cp -a out/buildroot/build/lib/ROX-CLib \$L/
		cp -a out/buildroot/build/apps/Pager out/buildroot/build/apps/OroboROX \$A/
		rm -rf \$L/ROX-CLib/src \$A/Pager/src \$A/Pager/build \
			\$A/OroboROX/src \$A/OroboROX/=build \
			\$A/OroboROX/Linux-ix86/OroboROX.dbg

		# Python: ROX-Lib2 in the lib directory, the apps as AppDirs.
		for t in $ROX_PYTHON_LIBS; do
			tar -C \$L --strip-components=1 -xjf downloads/\$t
		done
		for t in $ROX_PYTHON_APPS; do
			tar -C \$A --strip-components=1 -xjf downloads/\$t
		done
		chown -R root:root \$L \$A
	"
	step_check486
}

# Every program and library must run on a 486: no CMOV (i686) or
# CMPXCHG8B (i586). Debian 7 is built for the i486; this proves it, apart
# from the few libraries that choose code by CPU at run time, which are
# allowed by name in rootfs/check486.allow, each with its reason.
step_check486() {
	say "check486"
	as_root '
		n=0 bad=0 allowed=0
		for f in $(find out/rootfs -xdev -type f \( -perm -u+x -o -name "*.so*" \)); do
			head -c4 "$f" 2>/dev/null | grep -q ELF || continue
			n=$((n+1))
			case "$f" in
			# Copies the loader picks only on CPUs with those
			# features (AT_HWCAP): never on a 486.
			*/i586/*|*/i686/*|*/sse2/*) continue ;;
			esac
			objdump -d --no-show-raw-insn "$f" 2>/dev/null |
				grep -q -E "\scmov|cmpxchg8b" || continue
			if grep -q "^${f#out/rootfs}[[:space:]]" rootfs/check486.allow; then
				allowed=$((allowed+1))
			else
				echo "not for a 486: ${f#out/rootfs}"; bad=$((bad+1))
			fi
		done
		echo "$n programs and libraries, $bad not for a 486" \
			"($allowed allowed: rootfs/check486.allow)"
		[ $bad -eq 0 ]
	'
}

# FreeDOS's kernel and boot sector, FreeCOM and loadlin: out/dosboot
step_dosboot() {
	say "dosboot"
	in_container '
		set -e
		rm -rf out/dosboot && mkdir -p out/dosboot && cd out/dosboot
		unzip -q -j ../../downloads/ke2046_86f16.zip bin/kernel.sys
		unzip -q -j ../../downloads/freecom086-English.zip plainedt/command.com
		dpkg-deb --fsys-tarfile ../../downloads/loadlin_1.6f-6_amd64.deb |
			tar -xO ./usr/lib/loadlin/loadlin.exe.gz | gunzip > loadlin.exe
		# The FAT16 boot sector SYS installs, assembled from its source.
		unzip -q ../../downloads/ke2046s.zip "SOURCE/ke2046/boot/*"
		(cd SOURCE/ke2046/boot && nasm -f bin -dISFAT16 boot.asm \
			-o ../../../fat16.bin)
		rm -rf SOURCE
		ls -l
	'
}

step_disc() {
	say "disc: ${DISC_MB}MB"
	as_root "
		set -e
		python3 tools/mkdisc.py $DISC_MB out/bzImage out/rootfs \
			out/dosboot boot out/disc.img
		chown $(id -u):$(id -g) out/disc.img
	"
}

step_kernel() {
	say "kernel $KVER"
	mkdir -p "$KOUT"
	add_drivers
	in_container "
		set -e
		make -s -C $KSRC ARCH=i386 O=/work/$KOUT tinyconfig >/dev/null
		$KSRC/scripts/kconfig/merge_config.sh -m -O $KOUT \
			$KOUT/.config config/pccard.config >/dev/null
		make -s -C $KSRC ARCH=i386 O=/work/$KOUT olddefconfig
		# Every option asked for must have survived the dependencies.
		missing=
		for o in \$(sed -n 's/^\(CONFIG_[A-Z0-9_]*\)=y$/\1/p' config/pccard.config); do
			grep -q \"^\$o=y\" $KOUT/.config || missing=\"\$missing \$o\"
		done
		[ -z \"\$missing\" ] || { echo \"not set:\$missing\" >&2; exit 1; }
		make -s -j\$(nproc) -C $KSRC ARCH=i386 O=/work/$KOUT bzImage
	"
	cp "$KOUT/arch/x86/boot/bzImage" out/bzImage
	ls -l out/bzImage
}

steps=${*:-fetch container kernel rootfs rox dosboot disc}
for s in $steps; do
	case "$s" in
	fetch|container|kernel|rootfs|rox|check486|dosboot|disc) "step_$s" ;;
	*) echo "unknown step: $s" >&2; exit 2 ;;
	esac
done
