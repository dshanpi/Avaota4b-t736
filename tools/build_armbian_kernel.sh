#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
[ "$root" = /home/ubuntu/t736-mainline ]
src="$root/sources/work/linux"
out="$root/build/linux-armbian"
if [ ! -d "$src" ]; then
	cp -a --reflink=auto "$root/sources/snapshots/linux/linux-6.18.63" "$src"
fi
mkdir -p "$out" "$root/output" "$root/evidence/build"
cp "$root/boards/t736-mainline-bringup.dts" "$src/arch/arm64/boot/dts/allwinner/"
cp "$root/boards/t736_mmc_pio.c" "$src/drivers/mmc/host/"
python3 - "$src" <<'PY'
from pathlib import Path
import sys
root = Path(sys.argv[1])
edits = {
    'arch/arm64/boot/dts/allwinner/Makefile': '\ndtb-y += t736-mainline-bringup.dtb\n',
    'drivers/mmc/host/Makefile': '\nobj-$(CONFIG_MMC_T736_PIO) += t736_mmc_pio.o\n',
    'drivers/mmc/host/Kconfig': '\nconfig MMC_T736_PIO\n\ttristate "T736 SMHC3 initial PIO driver"\n\tdepends on OF && ARM64\n',
}
for name, text in edits.items():
    p = root / name
    s = p.read_text()
    if text.strip() not in s:
        p.write_text(s + text)
PY
if [ ! -f "$out/.config" ]; then
	cp "$root/build/linux-bringup/.config" "$out/.config"
fi
cfg="$src/scripts/config"
for symbol in ARCH_SUNXI MMC_T736_PIO MODULES MODULE_UNLOAD BINFMT_SCRIPT SYSVIPC POSIX_MQUEUE \
	CGROUPS MEMCG CGROUP_SCHED FAIR_GROUP_SCHED CFS_BANDWIDTH CPUSETS CGROUP_CPUACCT \
	CGROUP_DEVICE CGROUP_FREEZER CGROUP_PIDS CGROUP_BPF NAMESPACES UTS_NS IPC_NS USER_NS \
	PID_NS NET_NS EPOLL SIGNALFD TIMERFD AIO IO_URING INOTIFY_USER FANOTIFY KEYS \
	SECCOMP SECCOMP_FILTER BPF_SYSCALL FHANDLE PACKET UNIX INET IPV6 \
	NETFILTER UNIX_DIAG INET_DIAG TMPFS_POSIX_ACL TMPFS_XATTR EXT4_FS_POSIX_ACL \
	EXT4_FS_SECURITY BLK_DEV_LOOP BLK_DEV_RAM EFI_PARTITION PARTITION_ADVANCED \
	NLS NLS_UTF8 FAT_FS VFAT_FS OVERLAY_FS FUSE_FS AUTOFS_FS SECURITY SECURITYFS \
	CRYPTO_USER_API_HASH CRYPTO_USER_API_SKCIPHER CRYPTO_AES CRYPTO_SHA256 \
	CONFIGFS_FS IKCONFIG IKCONFIG_PROC HZ_PERIODIC KALLSYMS; do
	"$cfg" --file "$out/.config" --enable "$symbol"
done
"$cfg" --file "$out/.config" --set-str LOCALVERSION '-t736-armbian' \
	--disable LOCALVERSION_AUTO --disable CMDLINE_FORCE --enable CMDLINE_EXTEND \
	--set-str CMDLINE 'earlycon=uart8250,mmio32,0x02500000 console=ttyS0,115200 clk_ignore_unused'
make -C "$src" O="$out" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- olddefconfig
make -C "$src" O="$out" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j8 Image allwinner/t736-mainline-bringup.dtb modules
cp "$out/arch/arm64/boot/Image" "$root/output/Image-6.18.63-t736-armbian"
cp "$out/arch/arm64/boot/dts/allwinner/t736-mainline-bringup.dtb" "$root/output/t736-mainline-armbian.dtb"
sha256sum "$root/output/Image-6.18.63-t736-armbian" "$root/output/t736-mainline-armbian.dtb"
