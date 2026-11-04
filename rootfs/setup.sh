#!/bin/sh
# Run in the new root filesystem (chroot), after the overlay is copied in.
set -e
export LANG=C DEBIAN_FRONTEND=noninteractive

# One login, on tty1: each getty is memory the PC hasn't got.
sed -i 's/^\([2-6]:23:respawn\)/#\1/' /etc/inittab

# udhcpc, busybox's DHCP client, for ifupdown: dhclient is ten times its size.
ln -sf /bin/busybox /sbin/udhcpc
insserv apdswap

# No password for root: it is a single-user machine with no remote logins.
passwd -d root >/dev/null

# What the dpkg excludes would have left out, and apt's downloads.
find /usr/share/doc -mindepth 1 -not -name copyright -not -type d -delete
find /usr/share/doc -mindepth 1 -type d -empty -delete
rm -rf /usr/share/man/* /usr/share/info/* /usr/share/lintian/*
find /usr/share/locale -mindepth 1 -maxdepth 1 -not -name locale.alias \
	-exec rm -rf {} +
apt-get clean
rm -rf /var/lib/apt/lists/*
