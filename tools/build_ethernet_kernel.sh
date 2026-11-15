#!/bin/sh
# Build-only opt-in candidate; no board installation or reboot.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
[ "$root" = /home/ubuntu/t736-mainline ]
revision=11
export KBUILD_BUILD_VERSION=$revision
T736_ETHERNET=1 sh "$root/tools/build_armbian_kernel.sh"
# Target-native kernel header tools are not needed for this boot candidate.
# This profile keeps normal dependency checking for the image/libc packages.
DEB_BUILD_PROFILES=pkg.linux-upstream.nokernelheaders \
make -C "$root/sources/work/linux" O="$root/build/linux-armbian" \
	ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j8 \
	KDEB_PKGVERSION=6.18.63-$revision bindeb-pkg
cp "$root/build/linux-image-6.18.63-t736-armbian_6.18.63-${revision}_arm64.deb" "$root/output/"
cp "$root/build/linux-armbian/arch/arm64/boot/Image" "$root/output/Image-6.18.63-t736-armbian-v$revision"
cp "$root/build/linux-armbian/arch/arm64/boot/dts/allwinner/t736-mainline-bringup.dtb" "$root/output/t736-ethernet-v$revision.dtb"
