# T736 common-domain write observation

This SRAM experiment observes exactly three writes and a 10000 us delay. It does
not initialize DDR or access the controller, PHY, storage, PMIC, or DDR contents.
The DDR CCU must already have been prepared; this probe never repeats cold CCU.

Prerequisites are exact DDR CCU values `f9004a00/80000003/00010001` and common-domain
values `00003200/0/0`. The timer must first complete an independent 10000 us
pretest with at least 240000 counter ticks. Only then can the independent write
gate permit, in order, SYS_CFG `0x0a020010=0x2728`, CHANNEL_TYPE
`0x0a020014=2`, and PRCM `0x07010250=1`. The component reads back each write,
waits 10000 us, and reads the three values again. There are no retries or rollback.

`status=2, component_result=1` means only these writes, readbacks and wait were
observed. It proves no controller or PHY reachability. The successful control
return to BROM is zero, matching the previously tested SRAM entry convention.
A pretest failure reports `status=0x102, phase=4` and must contain zero domain
writes. The finite timer loop is unchanged; a failure is not permission to retry.

The raw payload is 3344 bytes at `0x48c00`; report is 128 bytes at `0x4e000`,
stack occupies `0x4f000..0x50000`. The static frame sum is 324 bytes. All ELF
LOAD memory stays in the accepted `[0x48c00,0x50000)` window.

Build offline with `tools/build_t736_domain_setup_probe.py`. The fixed host runner
`analysis/t736_domain_setup_fel_probe.py` pins source and ELF/raw hashes and checks
CP15 state, payload readback and zero report before execution. Only the root
hardware owner may execute it. Offline protocol/report tests and transport
adapter tests do not open hardware.

Final pre-hardware raw SHA-256:
`36c7e6c6c272c3d1080dbf19f7bb92dede1eaec9ed2c8446aa4acb0ce63bf1f1`.
Hardware observation passed on 2026-11-14: phase 6, component result +1,
pretest 240066 ticks, post-write delay 240080 ticks, exactly three writes from
`3200/0/0` to `2728/2/1`, and normal return to FEL. Evidence:
`mainline/evidence/t736-domain-setup-sram-hardware-20261114.json`. Tested source,
artifacts and host runner are archived in
`mainline/build/domain-setup-probe-tested-20261114/`. Do not rerun on this board
state; its initial-state guard is now intentionally unsatisfied.
