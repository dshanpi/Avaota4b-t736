#!/usr/bin/env python3
"""Check resolved kernel prerequisites for the T736 Armbian userspace."""
import argparse
from pathlib import Path


REQUIRED = {
    "SHMEM": "real shared memory and tmpfs instead of the ramfs fallback",
    "TMPFS": "bounded /run and /dev/shm with statfs capacity reporting",
    "TMPFS_POSIX_ACL": "systemd runtime directory permissions",
    "TMPFS_XATTR": "runtime filesystem extended attributes",
    "INET": "IPv4 networking",
    "IP_MULTICAST": "systemd-resolved IPv4 multicast socket options",
    "UNIX": "systemd local IPC",
    "NETDEVICES": "network device drivers",
    "FW_LOADER": "peripheral firmware loading infrastructure",
    "CRYPTO": "kernel cryptographic API dependencies",
    "EXT4_FS": "the eMMC root filesystem",
    "MMC_T736_PIO": "the initial eMMC host driver",
    "DEVTMPFS": "early device nodes",
    "ZONE_DMA32": "32-bit USB DMA allocations below 4 GiB on this 8 GiB board",
    "PINCTRL_T736_R": "USB1 PL6 VBUS enable GPIO",
    "REGULATOR_FIXED_VOLTAGE": "USB1 VBUS regulator",
    "CLK_T736_USB": "USB1 clock and reset control",
    "PHY_T736_USB2": "USB1 host PHY",
    "USB_EHCI_HCD_PLATFORM": "USB1 high-speed host",
    "USB_OHCI_HCD_PLATFORM": "USB1 full/low-speed host",
    "USB_STORAGE": "USB mass-storage validation",
}


def check(text):
    values = dict(line.split("=", 1) for line in text.splitlines()
                  if line.startswith("CONFIG_") and "=" in line)
    return [f"CONFIG_{name}=y required: {why}"
            for name, why in REQUIRED.items()
            if values.get("CONFIG_" + name) != "y"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("config", type=Path)
    args = parser.parse_args()
    errors = check(args.config.read_text())
    if errors:
        parser.exit(1, "\n".join(errors) + "\n")
    print(f"T736 Armbian kernel prerequisites passed ({len(REQUIRED)} symbols)")


if __name__ == "__main__":
    main()
