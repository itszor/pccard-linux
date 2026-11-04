#!/bin/sh
# Run in the new root filesystem (chroot), after the overlay is copied in.
set -e
export LANG=C DEBIAN_FRONTEND=noninteractive

# One login, on tty1: each getty is memory the PC hasn't got.
sed -i 's/^\([2-6]:23:respawn\)/#\1/' /etc/inittab

# udhcpc, busybox's DHCP client, for ifupdown: dhclient is ten times its size.
ln -sf /bin/busybox /sbin/udhcpc
insserv apdswap

# The kernel sets the clock from the CMOS at boot. hwclock would have no
# /dev/rtc (there is no RTC driver: see config/pccard.config), and !PC's
# CMOS clock follows RISC OS's, so there is nothing to write back to it.
insserv -r hwclock.sh

# No password for root: it is a single-user machine with no remote logins.
passwd -d root >/dev/null

# Today's root certificates, in place of Debian 7's: those are too old for
# most HTTPS sites now (0install's among them), and keep expired roots that
# this OpenSSL would build chains through.
mkdir -p /usr/local/share/ca-certificates/mozilla
awk '/BEGIN CERTIFICATE/ { n++; f = sprintf("/usr/local/share/ca-certificates/mozilla/%03d.crt", n) }
	f { print > f } /END CERTIFICATE/ { close(f); f = "" }' /tmp/cacert-*.pem
sed -i 's|^mozilla/|!mozilla/|' /etc/ca-certificates.conf
update-ca-certificates --fresh >/dev/null

# Icons for ROX's application directories (/usr/local/apps), from the
# programs' own.
ln -sf /usr/share/pixmaps/xterm-color_48x48.xpm /usr/local/apps/Terminal/.DirIcon
for i in /usr/share/pixmaps/dillo.png /usr/share/icons/hicolor/48x48/apps/dillo.png; do
	[ -e "$i" ] && ln -sf "$i" /usr/local/apps/Dillo/.DirIcon && break
done

# No udev: the machine's devices are fixed, and devtmpfs has them in /dev.
# X's packages bring it, but it costs memory and wants more of the kernel.
insserv -r udev-mtab 2>/dev/null || true
insserv -r udev

# No ssh host keys in the image: ssh-hostkeys makes them at first boot.
rm -f /etc/ssh/ssh_host_*
insserv ssh-hostkeys

# The user, logged in on tty1 without a password, whose .profile starts the
# desktop when booted with the framebuffer.
cat /etc/skel/.profile.desktop >> /etc/skel/.profile
rm /etc/skel/.profile.desktop
id pc >/dev/null 2>&1 ||
	useradd -m -s /bin/bash -c "PC card" -G audio,video,cdrom,floppy,plugdev pc
passwd -d pc >/dev/null
sed -i 's|^1:2345:respawn:/sbin/getty 38400 tty1$|1:2345:respawn:/sbin/getty --autologin pc 38400 tty1|' \
	/etc/inittab
grep -q -- '--autologin pc' /etc/inittab

# What the dpkg excludes would have left out, and apt's downloads.
find /usr/share/doc -mindepth 1 -not -name copyright -not -type d -delete
find /usr/share/doc -mindepth 1 -type d -empty -delete
rm -rf /usr/share/man/* /usr/share/info/* /usr/share/lintian/*
find /usr/share/locale -mindepth 1 -maxdepth 1 -not -name locale.alias \
	-exec rm -rf {} +
apt-get clean
rm -rf /var/lib/apt/lists/*
