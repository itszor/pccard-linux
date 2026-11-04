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
#   check486   check that everything in out/rootfs runs on a 486 (part of
#              rootfs, and can be run on its own)
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
PACKAGES=ifupdown,netbase,busybox,procps,kmod

# The disc image's size, in MB.
DISC_MB=${DISC_MB:-400}

# The boot partition: FreeDOS (kernel, and its source for the boot sector),
# FreeCOM, and Debian's loadlin, with their sources. All GPL.
FDK=https://github.com/FDOS/kernel/releases/download/ke2046
FDCOM=https://github.com/FDOS/freecom/releases/download/com086
DOWNLOADS="
$FDK/ke2046_86f16.zip ke2046_86f16.zip 3fc4adb1f693526089df3d46a6512f2c2201d6cffdda40eac4b99d666dcd356a
$FDK/ke2046s.zip ke2046s.zip 0ea71c9276aba7e1e371f42e8e17d4e9223ac4bedeacccce3ee700fc2866c5e3
$FDCOM/English.zip freecom086-English.zip 42093a00286f66bf922eebd8204a3b74b429f4c6973cd960ae1ab52ce80b4bd5
$FDCOM/freecom-source.zip freecom086-source.zip 4d9365772efc54fe5bd4bc48a27bcbb7041e71791af36dd0621ef4c8a34cc1d3
$MIRROR/pool/main/l/loadlin/loadlin_1.6f-6_amd64.deb loadlin_1.6f-6_amd64.deb 24bc7ec9c07d52080e0d19d3ec9e77441e9d52314bc98185e123eee51a59fd25
$MIRROR/pool/main/l/loadlin/loadlin_1.6f.orig.tar.gz loadlin_1.6f.orig.tar.gz eb77f58a24e3e545c172cb5f75a89bd8ea3392826aca582de080bb173d05a5bf
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

step_rootfs() {
	say "rootfs: Debian $SUITE"
	as_root "
		set -e
		rm -rf out/rootfs
		debootstrap --arch=i386 --variant=minbase --keyring=$KEYRING \
			--include=$PACKAGES $SUITE out/rootfs $MIRROR
		cp -r --preserve=mode rootfs/overlay/. out/rootfs/
		cp rootfs/setup.sh out/rootfs/tmp/setup.sh
		chroot out/rootfs /tmp/setup.sh
		rm out/rootfs/tmp/setup.sh
	"
	step_check486
}

# Every program and library must run on a 486: no CMOV (i686) or
# CMPXCHG8B (i586). Debian 7 is built for the i486; this proves it.
step_check486() {
	say "check486"
	as_root '
		n=0 bad=0
		for f in $(find out/rootfs -xdev -type f \( -perm -u+x -o -name "*.so*" \)); do
			head -c4 "$f" 2>/dev/null | grep -q ELF || continue
			n=$((n+1))
			if objdump -d --no-show-raw-insn "$f" 2>/dev/null |
			   grep -q -E "\scmov|cmpxchg8b"; then
				echo "not for a 486: $f"; bad=$((bad+1))
			fi
		done
		echo "$n programs and libraries, $bad not for a 486"
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

steps=${*:-fetch container kernel rootfs dosboot disc}
for s in $steps; do
	case "$s" in
	fetch|container|kernel|rootfs|check486|dosboot|disc) "step_$s" ;;
	*) echo "unknown step: $s" >&2; exit 2 ;;
	esac
done
