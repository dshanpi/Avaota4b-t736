# T736 source-only CCU SRAM test

This diagnostic prepares DDR clocks only. It does not initialize DRAM, run PHY
training, or prove Armbian boot. Root hardware testing passed on 2026-11-14:
`mainline/evidence/t736-ccu-cold-sram-hardware-20261114.json`.

The tested 4072-byte payload SHA256 is
`6639bc0d331f20b28ae2bb7aa16752fe31dbcb0bada2ff4f4fd8c9a92f6ee01d`.
An immutable task snapshot of the binary, ELF, build record and matching source
files is in `mainline/build/ccu-probe-tested-20261114/`. The build-time record
states `hardware_tested=false`; the adjacent `hardware-result.json` records
the subsequent successful board test and links its evidence.

The entry is `0x48c00`; the 128-byte report is at `0x4e000`; its private stack is
`0x4f000..0x50000`. The complete ELF memory footprint fits the accepted SRAM
window. Static stack-frame sizes sum to 620 bytes and there are no undefined
symbols. It reuses the tested source-only entry and v2 timer initialization.

The callback layer reads only six CCU registers. It permits writes only to
PLL_DDR, DRAM0_CLK and DRAM0_BGR, with a finite count bound. It rejects PHY,
controller, PMIC, storage and NSI/MBUS write requests. There is no UART/TWI
setup in this payload. The timestamp initializer may fill missing frequency
metadata or enable a stopped timestamp counter; it never clears the counter
or writes CPU CNTFRQ.

The observed result was completed stage 6, return 0, with 13 writes:

| Register | Before | After | Writes |
| --- | --- | --- | --- |
| PLL_DDR | `0x41304700` | `0xf9004a00` | 7 |
| DRAM0_CLK | `0` | `0x80000003` | 2 |
| DRAM0_BGR | `0x00000001` | `0x00010001` | 4 |
| NSI_CLK | `0xc3000000` | unchanged | 0 |
| NSI_BGR | `0x00010001` | unchanged | 0 |
| MBUS_CLK | `0x84000000` | unchanged | 0 |

`analysis/t736_ccu_cold_fel_probe.py` pins source and artifacts, binds the actual
USB interface and location, executes the small CP15 state check, verifies full
upload readback and a zeroed report, then executes once. It has no arbitrary
address or file argument. Errors stop the sequence; there is no automatic
retry or rollback. A subsequent domain/PHY test must account for the already
enabled DDR clocks, and must not repeat cold initialization blindly.
