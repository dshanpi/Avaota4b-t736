# License notices

Existing SPDX identifiers and upstream notices remain in the source and patches.
The U-Boot board files use GPL-2.0-or-later notices. The Linux board files use GPL-2.0-only notices.
The handoff candidate uses GPL-2.0-only notices.
`GPL-2.0.txt` is copied from the pinned official U-Boot source.
These notices do not grant rights to external vendor firmware, which is not included.

The current source-v6 backup additionally contains:

- Source SPL/storage/power components with their original SPDX notices.
- Linux display compatibility changes derived from the Tina GPL DRM sources. The original BSP baseline stays an external input; its import manifest and compatibility patch are included.
- A TF-A board-layout patch against the pinned community source. TF-A uses BSD-3-Clause; retain the upstream notices when obtaining the source.
- LPDDR4 command references checked against LiteDRAM. Its BSD-2-Clause license is retained in `spl/lpddr4/pmu/reference/LITEDRAM-LICENSE`.
- The recorded elvees ABI reference follows the GPL-2.0-or-later notices in its referenced source; provenance remains in the PMU audit files.

User authorization permits the fixed vendor DDR library for this board's runtime. It does not change redistribution rights. No DDR library, embedded training binary, vendor Boot0/BL31/U-Boot/OP-TEE/SCP, FES loader, disk image or rootfs is included here.
