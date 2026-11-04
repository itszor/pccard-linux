#!/bin/sh
# Build ROX's C applications, run in out/buildroot (a Debian 7 chroot with
# GTK's headers), with their sources in /build/src. Each builds itself as
# ROX applications do (AppRun --compile), for PLATFORM Linux-ix86: what
# their AppRun works out on any x86 Linux, the 5x86 included. The code is
# for the i486, as all of Debian 7 is.
#
#   /build/lib/ROX-CLib   the C library Pager uses
#   /build/apps/Pager     a pager applet for ROX-Filer's panel
#   /build/apps/OroboROX  ROX's window manager
set -e

export PLATFORM=Linux-ix86
export CFLAGS="-O2 -march=i486 -mtune=i486"
# Where ROX's libdir looks for ROX-CLib.
export LIBDIRPATH=/build/lib APPDIRPATH=/build/apps

SRC=/build/src
rm -rf /build/lib /build/apps
mkdir -p /build/lib /build/apps

echo "== ROX-CLib"
cd /build/lib
tar xzf "$SRC/ROX-CLib-2.1.10.tar.gz"
./ROX-CLib/AppRun --compile < /dev/null

echo "== Pager"
cd /build/apps
tar xzf "$SRC/Pager-1.2.tar.gz"
./Pager/AppRun --compile < /dev/null

echo "== OroboROX"
tar xjf "$SRC/OroboROX-0.9.8.1.tar.bz2"
mv OroboROX-0.9.8.1 OroboROX
./OroboROX/AppRun --compile < /dev/null

find /build/lib /build/apps -path '*/Linux-ix86/*' -type f -perm -u+x
