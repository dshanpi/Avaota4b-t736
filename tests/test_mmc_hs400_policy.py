#!/usr/bin/env python3
"""Check actual C HS400 policy, RO/reference integrity, and no-retry R1 rules."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
kernel = ROOT / "sources/work/linux-storage-v23"
if not kernel.exists():
    kernel = ROOT / "sources/linux"
mmch = (kernel / "include/linux/mmc/mmc.h").read_text()
core = (kernel / "drivers/mmc/core/mmc.c").read_text()
block = re.search(r"static int mmc_compare_ext_csds\(.*?\n}\n", core, re.S).group()
defines = dict((key, int(value)) for key, value in
               re.findall(r"^#define\s+(EXT_CSD_\w+)\s+(\d+)\b", mmch, re.M))
reference_offsets = [defines[key] + int(extra or 0) for key, extra in
                     re.findall(r"bw_ext_csd\[(EXT_CSD_\w+)(?: \+ (\d+))?\]", block)]
header = (ROOT / "boards/t736_mmc_hs400_tuning_draft.h").read_text()
actual_offsets = [int(value) for value in re.findall(r"\d+", re.search(
    r"t736_hs400_ro_offsets\[\] = \{(.*?)\}", header, re.S).group(1))]
assert actual_offsets == reference_offsets and len(actual_offsets) == 26
core_fields = re.findall(r"card->ext_csd\.([a-z_0-9]+(?:\[\d\])?)\s*==", block)
driver = (ROOT / "boards/t736_mmc_hs400_draft.c").read_text()
driver_ref = re.search(r"ref = \(struct t736_hs400_reference\) \{(.*?)\.hs_timing", driver, re.S).group(1)
driver_fields = re.findall(r"card->ext_csd\.([a-z_0-9]+(?:\[\d\])?)", driver_ref)
assert driver_fields == core_fields

prefix = "\n".join(line for line in mmch.splitlines() if re.match(
    r"#define\s+(R1_STATUS\(|R1_CURRENT_STATE\(|R1_READY_FOR_DATA\s|R1_SWITCH_ERROR\s|R1_STATE_TRAN\s)", line))
dma_header = (ROOT / "boards/t736_mmc_dma_desc.h").read_text()
prefix += "\n" + next(line for line in dma_header.splitlines() if line.startswith("#define T736_IRQ_ERRORS "))
SOURCE = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <stdio.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
#define BIT(n) (1U << (n))
''' + prefix + r'''
#include "t736_mmc_clock_hs200_draft.h"
#include "t736_mmc_hs400_policy_draft.h"
#include "t736_mmc_hs400_tuning_draft.h"

int main(void)
{
    struct t736_clock_plan plan;
    assert(!t736_plan_ddr8_transition_clock(50000000, 24000000, 800000000, &plan));
    assert(plan.card_hz == 50000000 && plan.reg == 0x81000003 && plan.m == 4);
    assert(!t736_plan_ddr8_transition_clock(25000000, 24000000, 800000000, &plan));
    assert(plan.card_hz == 25000000 && plan.reg == 0x81000007);
    assert(!t736_plan_ddr8_transition_clock(400000, 24000000, 0, &plan));
    assert(plan.card_hz == 400000 && plan.m == 15);
    assert(t736_plan_ddr8_transition_clock(50000001, 24000000, 800000000, &plan) == -EINVAL);
    assert(t736_plan_ddr8_transition_clock(399999, 24000000, 800000000, &plan) == -EINVAL);
    assert(!t736_plan_sdr_clock(100000000, 24000000, 800000000, &plan));
    assert(plan.reg == 0x81000003);
    assert(!t736_plan_sdr_clock(200000000, 24000000, 800000000, &plan));
    assert(plan.reg == 0x81000001);
    assert(T736_HS400_CTRL_DDR_VALUE == 0 && T736_HS400_NTSR_VALUE == 0);
    assert(T736_HS400_EDSD_VALUE == BIT(31) && T736_HS400_CRC_VALUE == 6);
    assert(T736_HS400_DRIVE_VALUE == BIT(16) && T736_HS400_INTERNAL_DIVIDER == 0);
    assert(T736_DDR8_TRANSITION_CTRL_VALUE == BIT(10));
    assert(T736_DDR8_TRANSITION_INTERNAL_DIVIDER == 1 && T736_DDR8_TRANSITION_DRIVE_VALUE == (BIT(16) | BIT(17)));
    const u32 rates[] = {400000,25000000,25000001,50000000,50000001,100000000,100000001,150000000,150000001,200000000};
    const unsigned commands[] = {0,0,32,32,32,32,36,36,14,14};
    const unsigned strobes[] = {0,0,33,33,26,26,17,17,13,13};
    for (unsigned i=0;i<10;i++) {
        struct t736_hs400_seed seed = t736_hs400_seed_for_rate(rates[i]);
        assert(seed.command == commands[i] && seed.strobe == strobes[i]);
    }
    u8 ext[512] = {0};
    struct t736_hs400_reference ref = {.hs_timing=3};
    ext[185] = 3; ext[183] = 6;
    for (unsigned i=0;i<26;i++) {
        ref.bytes[i] = 17+i;
        ext[t736_hs400_ro_offsets[i]] = ref.bytes[i];
    }
    assert(t736_hs400_extcsd_matches(&ref,ext));
    for (unsigned i=0;i<26;i++) {
        ext[t736_hs400_ro_offsets[i]] ^= 1;
        assert(!t736_hs400_extcsd_matches(&ref,ext));
        ext[t736_hs400_ro_offsets[i]] ^= 1;
    }
    ext[33] = 1; /* Mutable CACHE_CTRL is intentionally not part of RO reference. */
    assert(t736_hs400_extcsd_matches(&ref,ext));
    ext[185] = 2; assert(!t736_hs400_extcsd_matches(&ref,ext)); ext[185] = 3;
    ext[183] = 2; assert(!t736_hs400_extcsd_matches(&ref,ext));
    ext[183] = 0x86; assert(!t736_hs400_extcsd_matches(&ref,ext)); ext[183] = 6;
    ref.hs_timing = ext[185] = 0x13; assert(t736_hs400_extcsd_matches(&ref,ext));
    ext[185] = 3; assert(!t736_hs400_extcsd_matches(&ref,ext));
    u32 good_status = (4U << 9) | BIT(8);
    assert(t736_hs400_status_ok(good_status));
    assert(!t736_hs400_status_ok(good_status & ~BIT(8)));
    for (unsigned state=0;state<16;state++)
        assert(t736_hs400_status_ok((state << 9) | BIT(8)) == (state == 4));
    for (unsigned bit=0;bit<32;bit++) {
        if (BIT(bit) & (0xfff9a000U | BIT(7)))
            assert(!t736_hs400_status_ok(good_status | BIT(bit)));
        if (BIT(bit) & (T736_IRQ_ERRORS & ~(BIT(1)|BIT(6)|BIT(8))))
            assert(!t736_hs400_command_error_recoverable(-ETIMEDOUT, BIT(bit)));
    }
    assert(t736_hs400_command_error_recoverable(-EILSEQ, BIT(6)));
    assert(t736_hs400_command_error_recoverable(-EILSEQ, BIT(1)));
    assert(t736_hs400_command_error_recoverable(-ETIMEDOUT, BIT(8)));
    assert(t736_hs400_command_error_recoverable(-ETIMEDOUT, 0));
    assert(!t736_hs400_command_error_recoverable(-EILSEQ, 0));
    assert(!t736_hs400_command_error_recoverable(-EPROTO, BIT(6)));
    puts("PASS: HS400/DDR8 clocks+policy, seeds,26coreRO references/allbyte corruptions, realHS400mode/ES checks, R1 state+error mask, command-only error classification");
}
'''
with tempfile.TemporaryDirectory(prefix="t736-hs400-") as temporary:
    root = Path(temporary)
    source, binary = root / "test.c", root / "test"
    source.write_text(SOURCE)
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1",
                    "-fsanitize=address,undefined", "-I", str(ROOT / "boards"),
                    str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
