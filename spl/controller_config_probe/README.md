# Fixed controller geometry/timing experiment

This source-only SRAM experiment writes the existing checked 8-GiB geometry
and 1800-MHz timing subset. It performs exactly 22 geometry writes followed by
42 timing writes, with immediate readback of every write. It neither completes
the controller initialization sequence nor enters normal mode or trains PHY.

Before the first write it requires the six exact values
`f9004a00/80000003/00010001/2728/2/1` in DDR CCU/common-domain registers and
both controllers' MSTR/STAT/MRSTAT equal to `03040000/0/0`. The callback layer
independently requires the fixed address, value and order for every write.
Each write must receive its paired readback before another operation proceeds.
The three masked timing words per channel preserve their separately recorded
pre-write reserved fields. Any mismatch stops without retry or cleanup writes.

Phases are cold checks, layout, post-layout controller check, timing, final
controller/context check, and completed observation. Both post-layout and final
controllers must read `MSTR=03081002, STAT=0, MRSTAT=0`. Final clocks and common
domain must equal their initial exact values. The only allowed stores outside
SRAM are two MSTR words, twenty map words and forty-two timing words. The timing
subset includes static DFI pipeline parameters at offsets `580/584/588`; it
does not access CTL DFIMISC, SWCTL, PWRCTL, common ports, PHY or storage, release
additional resets, initiate DFI handoff or request normal mode.

The 512-byte report contains separate attempted/completed/verified counters,
all 64 immediate readbacks, six masked pre-write values, precise phase/status,
last addresses, and valid masks for the cold/final snapshots. `status=2` means
only geometry/timing observation succeeded. It does not establish usable DDR
or satisfy the remaining full-controller configuration/release prerequisites.

The raw image is 4744 bytes at `0x48c00`; report is `0x4e000..0x4e200` and
stack `0x4f000..0x50000`. The sum of all static stack frames is 1352 bytes;
no undefined symbols or external arithmetic runtime remain. Both ELF LOAD
segments stay inside the accepted SRAM window. The shared tested entry
temporarily installs/restores its abort vectors and disables/restores IRQ/FIQ.

The native ASan/UBSan test runs the actual C harness with mocked MMIO, injects
all 64 dropped writes, checks all twelve initial-state drifts and a final
INIT-state drift, and directly attacks the independent write gate. It emits
the real 512-byte success report used by Python ABI tests. Thirteen host tests
cover every actual readback/retained field, snapshot/count/phase contradictions,
payload/report initialization checks, explicit attempt markers and raw report
retention on decode failure, and bounded transfer failures. The updated
transport adapter's twelve tests also pass. These tests never open hardware.

`tools/build_t736_controller_config_probe.py` records all C/header/linker/test/
builder input hashes and ELF/raw/native-report hashes. The fixed host runner
`analysis/t736_controller_config_fel_probe.py` pins these, verifies the CP15
preflight and uploaded bytes, clears/verifies the report, then executes once.
Root alone owns hardware execution and recovery from any MMIO bus stall.

Raw SHA-256:
`3604572867077ac5e4507926e11c8314cb2bdc9737b516db2c566729f518bc78`.
Hardware observation is pending.
