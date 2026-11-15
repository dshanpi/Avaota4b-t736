# LPDDR4 PMU components

This directory is **not executable LPDDR4 training firmware**. No boot image is
built here, no PMU firmware is linked, and none of these components has been
executed on the T736 PHY. Compiling ARC objects does not validate training.

Implemented components:

* `commands.c` emits actual LPDDR4 MRW/MRR/MPC pin waveforms, including ZQ
  start/latch operands, from the public LiteDRAM truth table. Its cycles are
  successive **rising CK edges**. Type9's LPDDR4 slot-to-clock and CS mapping
  remains unknown, so these waveforms are not submitted to command RAM.
* `common_message.c` edits only LPDDR4 fields corroborated by the public Elvees
  message definition and T736 host-side DMEM addressing. It preserves all other
  bytes. It is not a complete DMEM constructor and must not be used with the
  existing LPDDR5 PMU image.
* `mailbox.c` receives 16-bit APB messages with bounded ready/acknowledgement
  polling, stream framing, argument limits and transport errors. It does not
  start, stop, upload or reset a PMU. A terminal `7` reports the firmware's
  claim; it is not proof that DDR works.
* `eye.c` selects a guarded contiguous passing window from samples already
  measured by a hardware backend. It does not measure an eye or modify delays.

Run `python3 spl/lpddr4/pmu/check.py` in `/home/ubuntu/t736-mainline`. The check
uses the unmodified public LPDDR4 header as an independent layout oracle,
exercises mailbox error/stream paths, compares 40,960 eye cases against an
exhaustive interval oracle under AddressSanitizer/UBSan, and cross-compiles
freestanding ARM32 and ARC EM4 objects. ARC object compilation enables the
multiply instruction used by the existing LPDDR5 compiler target; that target
capability has not been tested on this board by this check.

Run `python3 spl/lpddr4/pmu/check_commands.py` to verify all rank/MR/operand
combinations against pinned BSD-2-Clause LiteDRAM HDL truth tables. This also
compiles an exact test-only excerpt of the pinned public GPL LP5 encoder and
independently identifies its d3/d4 words as CA phases for MRW/MRR/MPC. See
`sequencer-audit.json` for the bit-level packing and precise remaining unknowns.
The public source's slot0/channel0 and slot1/channel1 comments are misleading:
its fast MRW path places consecutive MRW-1/MRW-2 commands in these two slots.

The separate `../mode_register.c` provides controller-mediated diagnostic MRR
for MR5/MR6/MR8 after DRAM is already initialized. It forces read commands and
checks type, rank, normal state, idle and bounded completion; it does not touch
PHY mux or MR13. Run `python3 spl/lpddr4/check_mode_register.py` for its native
failure-path checks and ARM32 compilation. `mode-register-audit.json` records
the public controller references and T736 receive-byte wiring. There is no
independently identified T736 receive-valid bit, so its raw results require
hardware cross-checking. It does not supply the missing initial PHY executor.

## Source and ABI limits

The test-only header under `reference/` is from
[Elvees ddrinit, commit 12ca43ab6964dece88c464f104af4d861a259a6b](https://github.com/elvees/ddrinit/blob/12ca43ab6964dece88c464f104af4d861a259a6b/include/dram/lpddr4/phy-training-params-lpddr4.h),
copyright 2021 RnD Center ELVEES, GPL-2.0-or-later. The GPL text is included.
Its byte offsets describe an older Synopsys LPDDR4 firmware interface, not all
of the T736 interface. T736's LP4 host writer shares the common frequency,
sequence, topology and MR-group offsets but uses additional fields through
0x546 and differs at offsets such as 0x0a. Those unknown fields are deliberately
not generated. No Elvees, Allwinner or other vendor executable array is included.

The mailbox protocol was independently implemented using the public Elvees
host protocol, the source-pinned A733 LPDDR5 ABI, and a read-only inspection of
T736's host mailbox register accesses. The T736 access width is 16 bits; Elvees
uses a different physical APB stride, which is not copied.

See `backend-audit.json` for exact remaining work and source references. In
particular the Type9 command sequencer, LP4 MRW/CBT/leveling/gating sequence,
measurement path, delay commit, and post-training PIE source remain absent.
The published LPDDR5 PMU's MR decoding, WCK/RDQS paths, seven-CA pin mapping and
private command descriptors cannot serve as an LPDDR4 implementation merely by
changing a memory-type constant or message offsets.
