# Making and using Debian 7 (wheezy) chroots: the root filesystem, and the
# chroot the ROX applications are built in. Sourced by build.sh's steps, in
# the build container as root, with SUITE, MIRROR and KEYRING set.

# wheezy_bootstrap DIR [PACKAGE,...]: a minimal Debian 7 for the i386
wheezy_bootstrap() {
	rm -rf "$1"
	debootstrap --arch=i386 --variant=minbase --keyring="$KEYRING" \
		${2:+--include=$2} "$SUITE" "$1" "$MIRROR"
}

# Debian 7's apt can't check its archive's expired keys. This checks the
# Release file with them as debootstrap does (gpgv), and the Packages file
# against the Release file; apt then checks each package against the
# Packages file.
wheezy_check_lists() {
	l=$1/var/lib/apt/lists
	r=$l/archive.debian.org_debian_dists_${SUITE}_Release
	p=$l/archive.debian.org_debian_dists_${SUITE}_main_binary-i386_Packages
	curl -sf -o /tmp/Release.gpg "$MIRROR/dists/$SUITE/Release.gpg"
	gpgv --keyring "$KEYRING" /tmp/Release.gpg "$r"
	want=$(awk '/^SHA256:/ { s = 1; next } /^[^ ]/ { s = 0 }
		s && $3 == "main/binary-i386/Packages" { print $1 }' "$r")
	got=$(sha256sum < "$p" | cut -d' ' -f1)
	if [ -z "$want" ] || [ "$want" != "$got" ]; then
		echo "Packages: sha256 $got, Release says $want" >&2
		return 1
	fi
	echo "package lists: Release signed by Debian, Packages matches it"
}

# wheezy_enter DIR: ready for apt in a chroot: the build machine's DNS,
# /proc, no services started (policy-rc.d), and checked package lists.
# wheezy_leave undoes it, but for the lists and resolv.conf.
wheezy_enter() {
	cp /etc/resolv.conf "$1/etc/resolv.conf"
	printf '#!/bin/sh\nexit 101\n' > "$1/usr/sbin/policy-rc.d"
	chmod 755 "$1/usr/sbin/policy-rc.d"
	mount -t proc proc "$1/proc"
	chroot "$1" apt-get update -q -o Acquire::Check-Valid-Until=false
	wheezy_check_lists "$1" || { wheezy_leave "$1"; return 1; }
}

wheezy_leave() {
	umount "$1/proc" 2>/dev/null || true
	rm -f "$1/usr/sbin/policy-rc.d"
}

# wheezy_install DIR PACKAGE...: without recommends. Unauthenticated to
# apt, as its keys have expired; wheezy_check_lists has checked them.
wheezy_install() {
	d=$1
	shift
	chroot "$d" env DEBIAN_FRONTEND=noninteractive apt-get install -q -y \
		--no-install-recommends -o APT::Get::AllowUnauthenticated=true "$@"
}
