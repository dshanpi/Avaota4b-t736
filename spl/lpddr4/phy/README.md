# T736 LPDDR4 PHY host components

`clocks.c` closes the DDR CCU isolation/configuration/enable sequence with an
already-valid independent fabric clock. It preserves **all NSI_CLK, NSI_BGR
and MBUS_CLK values**. It stops DRAM0, gates and resets its configuration bus,
starts DDRPLL, and then configures and enables DRAM0 with reset deassertion
before bus ungating.
Every module clock change, including a gate change, uses the documented UPD
handshake. An already-active matching configuration skips the handshake, so
the initial gated DRAM0 state does not depend on a stopped DDRPLL acknowledging
an unnecessary update. A stage report identifies the last fully completed step.
Only PLL_DDR, DRAM0_CLK and DRAM0_BGR are written. No fabric, DRAM/SMC or PHY
register is written here.

The source-only CCU component passed a real SRAM/FEL test on 2026-11-14:
`evidence/t736-ccu-cold-sram-hardware-20261114.json` records stage 6, result 0,
PLL_DDR `0x41304700 -> 0xf9004a00`, DRAM0_CLK `0 -> 0x80000003`, and
DRAM0_BGR `1 -> 0x00010001`. The respective write counts were 7, 2 and 4.
All three fabric registers were unchanged and execution returned to FEL.
The confirmed 10 us timer delay consumed 318 counter ticks at the platform's
24 MHz effective rate; raw CNTFRQ remained zero. This verifies the CCU sequence
in the observed early-Boot0/FEL context, without any PHY, DRAM or storage access.
It does not establish DRAM initialization or independent normal boot.

The real early-Boot0/FEL observation on ROM ID `0x190300` reports NSI_CLK
`0xc3000000`, NSI_BGR `0x00010001`, MBUS_CLK `0x84000000`. The manual identifies
NSI source 3 as PERI0_600M and MBUS source 4 as PERI0_400M; neither is DDRPLL.
The component accepts these sources or SYS24M (source 0 for both), keeps them
unchanged to preserve the FEL connection, and rejects other sources, DFS,
pending updates, missing fabric gates/reset release or an active controller.
It configures DDRPLL=1800 MHz and DRAM0=450 MHz. Actual PERI0 frequency has not
been measured and must already be valid. The observed early-Boot0 setup is
**not** a completed independent clock tree. Normal source boot may use SYS24M
or a separately source-initialized PERI0 PLL.

The manual identifies `0x02002580` as **NSI_CLK**, `0x02002584` as **NSI_BGR**,
and `0x02002588` as **MBUS_CLK**. Names used by the inspected public T736 driver
are misleading here. The current code uses the manual's register names.

`host.c` implements a cold-start 1800 MHz PLL component, reset-before-ownership
acquisition, four checked eight-selector DQ permutations, and Pstate-0/1:4 DFI
clock selection. It also provides the conservative non-clearing ownership
observer used as the live-board probe contract. It does not claim complete DRAM
initialization or emit any PMU/PIE executable content.

Every PHY access is a halfword. CCU/controller accesses are words. Callbacks
return errors, including the delay callback: a stalled timer cannot silently
satisfy a minimum PLL settling interval. Status polls have finite limits, and
the test harness injects failure at every IO position in each successful flow.

The PLL component can start the PLL from a reset/gated state; it does not depend
on an already-running vendor PLL routine. The caller must first isolate all
DRAM clients and disable the DRAM0 external clock and configuration bus gates.
NSI/MBUS remain on the supported independent sources, with no DFS or update
pending. This component checks those conditions before its first write.
It preserves unrelated PLL fields and deliberately writes no read-only lock bit.
LDO is enabled before divider parameters and PLL enable. Lock must be observed
three consecutive times, then an additional confirmed 20 microseconds must
elapse before the output gate is enabled. A fresh read rejects lost lock after
that settling interval. These timings follow manual 4.1.4.1 Method 2; all
downstream DDRPLL consumers remain gated during its output-gate transition.
The test includes slow lock acquisition exceeding 20 microseconds, timer
failure during the post-lock interval, and lost lock before output enable.

Bit24 is a **board-specific opaque control** corroborated by the public T736
driver, the SDK host interface, and the observed board value. Its undocumented
meaning is not inferred. A lock-clear timeout, lock-set timeout, IO failure or
timer failure stops the sequence. The real CCU test covers this PLL path;
electrical frequency measurement and PHY training are separate validations.

`cold_claim`, `set_dq_selectors` and `select_pstate0` additionally require both
controllers in init mode with the observed two-channel/two-rank LP4 MSTR,
enabled controller bus and a stable 1800 MHz PLL. `cold_claim` asserts reset and
stall, verifies them, then changes APB ownership. The other writes require host
ownership and reset+stall already asserted. They are intended solely for SRAM
cold bring-up with DMA/interrupts/other DRAM clients stopped. These functions
must never be called from the running system to obtain a register dump.

For a running system, `observe_host` reads only `0x0aaa0000` as a 16-bit value.
When the result is nonzero it stops without touching any other PHY register.
Only with host ownership does it read MicroReset at `0x0aaa0132`, also 16-bit.
Neither is read-to-clear. No mailbox ACK or ownership write occurs. The actual
board reported owner 1 and took the refusal path; this is not validation of the
new cold-start writes.

DQ inputs are selectors **already in PHY CSR order**, not schematic net lists.
No board preset is supplied. DQS swapping and CA mapping require separate
verified contracts. ARC internal addresses cannot be translated to host MMIO
by a uniform base subtraction, and this code contains no such conversion.

Run `python3 spl/lpddr4/phy/check.py` from the isolated Linux project. It builds
the native ASan/UBSan fault tests and an ARM32 freestanding object under
`build/lpddr4-phy-host/`. Source/object hashes and test output go to
`evidence/lpddr4-phy-host-check.json` and `.log`. This does not touch the board.
Earlier in-source build/report files are historical outputs, not source inputs
or current validation evidence.
When both real read-only CCU observation files are present, the checker also
generates native test input from their exact six values, verifies ROM ID and
read-only/guard-success status, and tests that the three fabric values survive
the complete simulated DDR clock sequence unchanged. When the real CCU write
report is present, the checker also validates its raw report, preserved fabric,
write counts, pinned artifact and current source hashes before recording the
CCU hardware pass. PHY ownership/training writes remain untested.

The remaining physical work includes PRCM/DRAMC and SMC setup, PHY analog
configuration and impedance calibration, LP4 command/RESET_n/CKE control,
training measurements, and source-authored post-training sequencing. The host
branch candidates recorded in `provenance.json` are deliberately not runtime
register writes until their meanings and LP4 behavior are established.

The inspected SDK LP4 channel setup applies the dual-channel/reset fields to
DRAMC_SYS_CFG at `0x0a020010` and type/channel selection at `0x0a020014`.
The public driver's additional `0x07010310`/`0x07410310` PRCM writes have not
been corroborated by that LP4 host path and are not copied. Its memory-isolation
control at `0x07010250` and secure-chip SMC workaround also require a separate
reviewed contract. CCU completion must not bypass those steps or a real LP4
command/training executor.
