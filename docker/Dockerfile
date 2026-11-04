# Builds Linux 4.19 for the PC card's 486 (buster's gcc 8, of 4.19's era),
# a Debian 7 root filesystem with debootstrap, and the disc image.
FROM debian/eol:buster
RUN apt-get update && \
    apt-get install -y --no-install-recommends gcc make bc bison flex \
        libc6-dev xz-utils cpio kmod libelf-dev \
        debootstrap debian-archive-keyring e2fsprogs dosfstools mtools \
        fdisk syslinux syslinux-common python3 nasm unzip && \
    apt-get clean
WORKDIR /work
