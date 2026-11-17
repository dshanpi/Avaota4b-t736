# VE 400 MHz candidate and DCDC2 read-only diagnostics

The clock component changes the decoder's existing PERI0_800M divider from
4 to 2 and the matching fixed-rate checks/messages. A separate, subsequently
authorized diagnostic component adds a read-only `dcdc2_snapshot` sysfs file
to the existing PMIC owner. It performs no transaction at boot; an explicit
read obtains the current programmed DCDC2 setpoint under the existing owner
and bus mutexes. Neither component changes PLLs, voltage settings, DDR, GPU,
storage, DMA ownership, reset or power sequencing.

The accepted v27 files and outputs remain unchanged. `manifest.json` pins the
two accepted clock/power source hashes; `media-v28-ve400.patch` remains the
unchanged clock component. `dcdc2-readonly-manifest.json` and
`dcdc2-readonly-snapshot.patch` describe the third source file,
`drivers/regulator/t736-gpu-supply.c`. `combined-manifest.json` ties both
components together. The preliminary clock-only artifacts were preserved and
the combined incremental kernel/deb/PVR build passed. Its fixed artifact hashes
and checks are in `evidence/media-v28-combined-build-20261117.json`. There is no
hardware result for this candidate yet.

Before any build, root decides whether the WC-copy benchmark still justifies
400 MHz and reviews the separate voltage audit. A manufacturer OPP entry is
design evidence, not a measurement of this board's actual rail voltage.

If root starts a build later:

1. Check free space. At preparation time the build volume had approximately
   57 GiB available; no kernel tree or output directory was copied here.
2. Create an independent source copy from
   `/home/ubuntu/t736-mainline/sources/work/linux-media-v27` to
   `sources/work/linux-media-v28`. Use a full copy or a filesystem reflink,
   never a hardlink copy whose edits could change v27.
3. Verify the two baseline source hashes, apply `media-v28-ve400.patch` only
   inside the new source copy, and verify the candidate source hashes. Check
   that the storage, GPU, DT and Cedar DMA sources still match v27. Apply the
   separate DCDC2 patch to its pinned PMIC source baseline and verify its
   candidate SHA. Do not patch an actively compiling source tree: complete the
   current build, preserve its record, then apply and rebuild the combined
   candidate. The original CPU snapshot ABI and PMIC write paths are unchanged.
4. Create a separate `build/linux-media-v28` and seed only its `.config` from
   `build/linux-media-v27/.config`. Set `CONFIG_LOCALVERSION=-t736-media-v28`,
   `KBUILD_BUILD_VERSION=28` and package version `6.18.63-28`. Run olddefconfig
   and verify that the local version is the only intended config difference.
5. Build ARM64 Image, the existing `allwinner/t736-gpu-candidate.dtb`, modules
   and package in the new output. Rebuild the matching PVR module into
   `build/pvr-bxm-media-v28` against the v28 kernel. Record source/config/image/
   DTB/module/package hashes and check dependencies before deployment.
6. Root performs any installation or hardware tests. Keep the accepted v27
   boot artifacts available for rollback. Before starting a decode experiment,
   read the bound `t736-gpu-supply` device's `dcdc2_snapshot` sysfs attribute.
   It reports raw ID/enable/selector/phase/feedback plus `setpoint_uv`; this is
   the PMIC setpoint, not an ADC or physical rail measurement. Disabled DCDC2,
   reserved selectors and DCDC2/3 multiphase mode are rejected. Keep all rail
   settings unchanged. Verify400MHz from the actual CCU and
   CCF rate, unchanged parent PLL and voltage, exact decoded frames, IRQs,
   pinned-memory use, reset/error behavior and comparative throughput.

The existing ownership check deliberately rejects an already-enabled clock
with the old200MHz divider. Do not bypass it or retime a live gate. A fresh boot
with no conflicting inherited owner is required for this candidate.

Do not invoke `tools/build_cedar_wc_kernel.sh` for this candidate: that script
is specifically bound to the accepted v27 source/output paths.
