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
cp "$root/boards/pinctrl-t736-r.c" "$src/drivers/pinctrl/sunxi/"
cp "$root/boards/clk-t736-usb.c" "$src/drivers/clk/"
cp "$root/boards/phy-t736-usb2.c" "$src/drivers/phy/allwinner/"
python3 - "$src" <<'PY'
from pathlib import Path
import sys
root = Path(sys.argv[1])
edits = {
    'arch/arm64/boot/dts/allwinner/Makefile': '\ndtb-y += t736-mainline-bringup.dtb\n',
    'drivers/mmc/host/Makefile': '\nobj-$(CONFIG_MMC_T736_PIO) += t736_mmc_pio.o\n',
    'drivers/mmc/host/Kconfig': '\nconfig MMC_T736_PIO\n\ttristate "T736 SMHC3 initial PIO driver"\n\tdepends on OF && ARM64\n',
    'drivers/pinctrl/sunxi/Makefile': '\nobj-$(CONFIG_PINCTRL_T736_R) += pinctrl-t736-r.o\n',
    'drivers/pinctrl/sunxi/Kconfig': '\nconfig PINCTRL_T736_R\n\tbool "T736 always-on GPIO banks"\n\tdepends on ARCH_SUNXI && OF\n\tselect PINCTRL_SUNXI\n',
    'drivers/clk/Makefile': '\nobj-$(CONFIG_CLK_T736_USB) += clk-t736-usb.o\n',
    'drivers/clk/Kconfig': '\nconfig CLK_T736_USB\n\tbool "T736 USB1 initial clock/reset subset"\n\tdepends on ARCH_SUNXI && OF && COMMON_CLK\n\tselect RESET_CONTROLLER\n',
    'drivers/phy/allwinner/Makefile': '\nobj-$(CONFIG_PHY_T736_USB2) += phy-t736-usb2.o\n',
    'drivers/phy/allwinner/Kconfig': '\nconfig PHY_T736_USB2\n\tbool "T736 USB1 host PHY"\n\tdepends on ARCH_SUNXI && OF && COMMON_CLK && RESET_CONTROLLER && REGULATOR\n\tselect GENERIC_PHY\n',
}
for name, text in edits.items():
    p = root / name
    s = p.read_text()
    if text.strip() not in s:
        p.write_text(s + text)
PY
if [ ! -f "$out/.config" ]; then
	cp "$root/userpatches/linux-t736-mainline-current.config" "$out/.config"
fi
cfg="$src/scripts/config"
for symbol in ARCH_SUNXI MMC_T736_PIO MODULES MODULE_UNLOAD BINFMT_SCRIPT BINFMT_MISC SYSVIPC POSIX_MQUEUE \
	CGROUPS MEMCG CGROUP_SCHED FAIR_GROUP_SCHED CFS_BANDWIDTH CPUSETS CGROUP_CPUACCT \
	CGROUP_DEVICE CGROUP_FREEZER CGROUP_PIDS CGROUP_BPF NAMESPACES UTS_NS IPC_NS USER_NS \
	PID_NS NET_NS EPOLL SIGNALFD TIMERFD AIO IO_URING INOTIFY_USER FANOTIFY KEYS \
	SECCOMP SECCOMP_FILTER BPF_SYSCALL FHANDLE PACKET UNIX INET IP_MULTICAST IPV6 \
	NETDEVICES FW_LOADER NETFILTER UNIX_DIAG INET_DIAG SHMEM TMPFS TMPFS_POSIX_ACL TMPFS_XATTR EXT4_FS_POSIX_ACL \
	EXT4_FS_SECURITY BLK_DEV_LOOP BLK_DEV_RAM EFI_PARTITION PARTITION_ADVANCED \
	NLS NLS_UTF8 NLS_CODEPAGE_437 NLS_ISO8859_1 FAT_FS VFAT_FS FAT_DEFAULT_UTF8 \
	OVERLAY_FS FUSE_FS AUTOFS_FS SECURITY SECURITYFS \
	CRYPTO CRYPTO_USER_API_HASH CRYPTO_USER_API_SKCIPHER CRYPTO_AES CRYPTO_SHA256 \
	CONFIGFS_FS IKCONFIG IKCONFIG_PROC HZ_PERIODIC KALLSYMS \
	ZONE_DMA ZONE_DMA32 PINCTRL GPIOLIB PINCTRL_T736_R REGULATOR REGULATOR_FIXED_VOLTAGE \
	COMMON_CLK CLK_T736_USB RESET_CONTROLLER GENERIC_PHY PHY_T736_USB2 \
	USB_SUPPORT USB USB_EHCI_HCD USB_EHCI_HCD_PLATFORM USB_OHCI_HCD USB_OHCI_HCD_PLATFORM \
	SCSI BLK_DEV_SD USB_STORAGE HID HID_GENERIC USB_HID INPUT_EVDEV DEBUG_FS; do
	"$cfg" --file "$out/.config" --enable "$symbol"
done
"$cfg" --file "$out/.config" --set-str LOCALVERSION '-t736-armbian' \
	--disable LOCALVERSION_AUTO --disable CMDLINE_FORCE --enable CMDLINE_EXTEND \
	--set-str CMDLINE 'earlycon=uart8250,mmio32,0x02500000 console=ttyS0,115200 clk_ignore_unused'
make -C "$src" O="$out" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- olddefconfig
# Check resolved Kconfig, including dependencies: scripts/config alone can
# silently lose requested children when a parent is disabled.
python3 "$root/tools/check_kernel_config.py" "$out/.config"
make -C "$src" O="$out" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j8 Image allwinner/t736-mainline-bringup.dtb modules
cp "$out/arch/arm64/boot/Image" "$root/output/Image-6.18.63-t736-armbian"
cp "$out/arch/arm64/boot/dts/allwinner/t736-mainline-bringup.dtb" "$root/output/t736-mainline-armbian.dtb"
sha256sum "$root/output/Image-6.18.63-t736-armbian" "$root/output/t736-mainline-armbian.dtb"
