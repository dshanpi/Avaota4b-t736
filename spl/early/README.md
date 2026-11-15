# T736 source-only SRAM probe (2026-11-14)

This is a callable AArch32 FEL diagnostic, **not a complete SPL, DDR initializer,
Armbian boot image, or evidence of a successful cold boot**. It contains no
embedded Tina executable, PMU instruction array, DDR library, BL31, SCP, or
OP-TEE. It does not access DRAM or storage and sends no PMIC register writes.
The standalone TWI write API is available for explicit future rail operations;
the diagnostic does not call it, and the linker removes it from the raw probe.

## Entry and memory contract

- Raw load address and execute entry: `0x00048c00`. There is no eGON header;
  do not add the `+0x40` offset used by SyterKit's header-bearing eFEX image.
- Proposed reserved SRAM window: `[0x00048c00, 0x00050000)`, entirely inside SRAM_A2.
  The CPUS-only first 16 KiB and NPU/shared SRAM are not used.
- Fixed report: `0x0004e000`, 64 bytes. Stack: `[0x0004f000, 0x00050000)`.
- Caller must provide secure AArch32 ARM SVC execution, physical addressing,
  readable physical counter, MMU and data cache disabled, and low vectors.
  Entry returns `r0 = 0x100` immediately for a detectable mode/MMU/data-cache/
  high-vector violation. This rejection does not initialize the report.
- The FEL transport must ensure instruction-cache coherency after upload and
  before the first instruction at `0x48c00`: either instruction cache is already
  disabled, or the transport performs the required cache invalidation and
  instruction synchronization. The probe allows I-cache enabled and cannot prove
  that its own first instructions are fresh. A cache-maintenance operation inside
  the newly uploaded entry alone would not establish that prerequisite.
- Caller must first ensure this entire reserved window is free of its own FEL
  buffers, code, and saved context. The entry address is corroborated by
  SyterKit's sun60iw2 eFEX configuration; that is **not proof of the undocumented
  T736 BootROM's entire working-memory footprint**. A T736-capable transport and
  verified execution contract are hardware-test prerequisites.
- The probe saves/restores `r4-r11`, caller SP/LR, CPSR control and VBAR using
  its own SRAM context. It never pushes onto the caller's stack. It does not
  change cache/MMU configuration. An abort/undefined exception records
  `0xdead0001` at report offset 8 and halts; recovery then requires reset.
- UART0 and R-TWI0 pinmux/clocks are configured for the diagnostic and remain
  configured on return. It refuses an active TWI packet/DVFS master; this probe
  is intended for early FEL, not injection into a running OS.

## What executes

1. Establish the SRAM stack, exception vectors and zeroed BSS.
2. Set PB9/PB10 function 2, UART0 source 24 MHz, divisor 13, 115200 baud 8N1.
3. Observe raw `CNTFRQ`, timestamp EN/FREQID and physical-counter advancement.
   BootROM may leave `CNTFRQ=0` while the counter is already running. V2 uses
   the platform's fixed 24 MHz convention, fills only missing FREQID metadata
   at `0x08020020`, and sets EN at `0x08020000` only if disabled. It writes no
   CP15 frequency register or counter value. Nonzero conflicting frequency
   metadata fails. Readbacks and an advancing CNTPCT are required; reads use
   ISB before MRRC. The reported effective rate is not an oscillator measurement.
   All polling has finite-iteration bounds, including a stopped-counter escape.
4. Set PL0/PL1 function 2, APBS1 SYS_CLK24M/div1, R-TWI0 100 kHz. The byte
   engine checks every status, uses W1C interrupt semantics, sends a final NACK
   on reads, and checks STOP completion. Arbitration loss does not send STOP.
   A stuck bus is reported without attempting manual recovery clocks.
5. Read AXP8191 address `0x36`, ID register `0x0e` (expected `0x03`), then
   raw registers `0x10, 0x19, 0x20, 0x27, 0x22, 0x3a`. No voltage is changed.
6. Print status and return to the caller. DDR is explicitly reported untested.

The current report is 16 little-endian words: magic `0x54373336`, ABI version `2`,
status, effective hardware counter rate, 10-us-delay ticks, PMIC ID, last TWI
status, cleanup error, six PMIC register values in the above order, entry SCTLR,
raw CPU CNTFRQ. The last word was reserved in historical ABI version 1.
Status `1` means entered, `2` means the diagnostic reads succeeded; neither
means DDR or system boot succeeded. `0x101` is counter validation failure,
`0x102` is delay failure, `0x103` is active autonomous TWI, `0x200 + -error`
reports a TWI/PMIC failure (`error -8` is unexpected PMIC ID).

## Build and validation

Run `python3 mainline/tools/build_t736_sram_probe.py` on a Linux build host with
`arm-linux-gnueabi-gcc` and a native C compiler. Outputs are restricted to
`mainline/build/sram-probe/`. The script records source and output hashes and
compiler commands, validates entry/LOAD bounds and absence of undefined symbols,
and checks compiler-reported static stack frames against the 4 KiB stack.
Native TWI simulations cover successful reads/writes, every-phase NACK,
arbitration and timeout, repeated START, W1C, STOP failure, stopped/wrapped
counter, invalid requests, bus errors, and refusal of active DVFS/packet engines.
The additional timer host tests cover running counter with raw CNTFRQ zero,
missing metadata, enable, stopped counter, wrap, conflicting frequency metadata,
failed write readbacks, and no counter-value/CNTFRQ writes.

## Larger probe hardware results

Root tested the v1 3088-byte probe successfully through SRAM upload/readback,
entry, UART, private stack and return, but its incorrect `CNTFRQ != 24 MHz`
guard stopped with status `0x101`. It reached no TWI/PMIC transaction. The
original binary/ELF/build record remain in `mainline/build/sram-probe-v1-20261114/`
(binary SHA256 `6cb1752b8d4aba4cd2f4f87ffa8d27a467fe12cf3c3fe3d2ddadbf1fd999ef74`).
Evidence: `mainline/evidence/t736-early-sram-hardware-20261114.json`.

The corrected source-only **v2 3960-byte probe passed on the board** with binary
SHA256 `4b02c27e59e52958c9c782ec5e56718225dc0bd084a3ea3425f982981ed9dc8c`.
Evidence: `mainline/evidence/t736-early-v2-sram-hardware-20261114.json` and its
serial log. Timestamp EN was already 1 and the counter already advanced;
FREQID changed from 0 to `0x016e3600` while raw CNTFRQ remained 0. The 10-us
delay observation was 321 ticks. AXP8191 ID was `0x03`; registers
`10/19/20/27/22/3a` read `ff/bc/0f/0d/46/06`. Last TWI status was `0x58`,
cleanup error 0, and return to FEL succeeded. No PMIC register was written.
This validates the early timer, UART, SRAM stack and PMIC read path; **DDR
initialization and Armbian boot are not established by these results**.

## Hardware observation: smaller independent probe

On 2026-11-14 root successfully uploaded, read back, executed and retrieved the
report from the separate **168-byte, stackless** CP15 probe in
`analysis/transport_probe_20261114/`. Its complete loaded and report range was
`[0x48c00, 0x48ca8)`. The payload SHA256 matched
`2b1dd03fb26940103c057f7f40cd20874566314da3e7f50a78dd2f9f5d2d88d9`.
Evidence: `mainline/evidence/t736-cp15-sram-hardware-20261114.json`.

The actual board reported raw BROM ID `0x00190300`, mode 1, firmware 1,
scratch pointer `0x000ee800`, CPSR `0x60000153` (SVC), entry SP `0x000f2200`,
return LR `0x0001a138`, SCTLR `0x00c50838` (MMU, D-cache and I-cache all off),
and MIDR `0x412fd050` (ARM Cortex-A55 r2p0). This is an observed ROM identity;
the xfel `t736.c` value `0x00170800` and the sunxi-tools old-T7 SRAM map are
not this board's execution contract. The source-only probe accessed no MMIO,
PMIC, DDR or storage, and wrote no CP15 register.

The Windows transport used the existing `usbUDisc` driver, SetupAPI-bound
actual interface device/LocationPaths and overlapped vendor bulk IOCTLs.
AWUS and FEL status checks passed. No driver was replaced. Its 4 KiB exclusion
around the reported scratch pointer is a conservative client policy, **not a
measurement of the ROM scratch allocation size**.

This successful test validates only the small probe and its own memory range.
By itself it does **not** validate the original larger 3088-byte UART/TWI executable,
the report at `0x4e000`, the stack at `0x4f000..0x50000`, timer accesses,
MMIO clock/pinmux writes, PMIC communication, LPDDR4 initialization, or Armbian
boot. Further probes should initially reuse the exact small tested range;
enlarging the range requires separate memory-footprint evidence. The timer and
pure clock-register observations can be split into independent stackless
payloads; PMIC reads also require TWI controller/clock/pinmux operations and
need their own staged validation.

The subsequent wider-window review is recorded in
`analysis/evidence/t736-fes-sram-window-review-20261114.json`. The matching
golden FES has a `0x371c0`-byte header/file length and run address `0x48c00`;
the normal OpenixCLI/libefex path uploads that complete contiguous buffer
before execution, ending at `0x7fdc0`. A successful prior board FES task is
archived. Its entry uses the original BROM stack and returns through saved LR;
it does not swap a ROM working region before entry. Thus this proposed
`[0x48c00,0x50000)` code/report/stack range fits wholly within the prior FES
upload, away from the now-observed ROM scratch and stack. This additional
evidence supports a bounded test of the larger source probe; it does not
pretend its UART/TWI/timer behavior has already been hardware-tested. The
review distinguishes public host-source behavior from an unavailable historical
USB packet capture and installed-binary source revision.

## Register provenance

Independent C/assembly implementation; no upstream file or instruction sequence
is embedded as an opaque payload. Register facts were checked against the T736
User Manual V0.90, memory map and sections 4.1/4.2 (CCU/PRCM), 18.11.10 (S_TWI0),
18.14 (main PIO) and R-PIO register descriptions. Board pin/PMIC connectivity is
documented in `../../docs/HARDWARE_MAP.md`. T736 main PIO bank stride is 0x80;
R-PIO uses the separate layout. No global pad/LDO preset was copied.

SyterKit was consulted at commit
`7f539e17762639589897db835e58dd638b2bea80`:

| File | Git blob SHA | Fact cross-check |
| --- | --- | --- |
| `soc/sun60iw2/configs/lpddr4_efex_defconfig` | `5a027b6dc5abb5163065e54227e708d664b1de37` | eFEX SRAM base `0x48c00` |
| `arch/arm/efex.S` | `c7a4ac32408feb82931d1f6c4210265022f575db` | ARM entry/return convention |
| `arch/arm/timer.c` | `495816b541b785121b08576728eccd9b3f2ad9c9` | physical counter instruction |
| `drivers/i2c/i2c.c` | `11e06e4b43b41d473aaa3aa3edf08fdd9a250858` | byte-engine state sequence |
| `dts/include/dt-bindings/soc/sun60iw2.h` | `7640248dc9536ebe1589d4e335e1cc24afb40e42` | MMIO base addresses |
| `soc/sun60iw2/app_efex/init_dram/main.c` | `c2c0a29490079c5d0e08bdfd2d1dca63610b6b76` | UART/R-TWI pin assignments |

No Radxa source or binary is used. SyterKit's broader maximum-size allowance and
board-specific global pad/LDO changes are intentionally not imported into this
small diagnostic.
