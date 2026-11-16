#!/usr/bin/env python3
"""Exercise actual HS clock math against source-selection and overspeed risks."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = r'''
#include <assert.h>
#include <stdint.h>
#include <errno.h>
#include <stdio.h>
typedef uint32_t u32;
typedef uint64_t u64;
#define BIT(n) (1U << (n))
#include "t736_mmc_clock_hs_draft.h"

int main(void)
{
    /* 26M * 12 / 13 = 24M; 24M * 100 / 3 = 800M. */
    u32 ref = BIT(31) | BIT(28) | BIT(27) | (11U << 8) | (12U << 16);
    u32 peri = BIT(31) | BIT(28) | BIT(26) | BIT(24) | (99U << 8) | (2U << 16);
    struct t736_clock_plan p;
    assert(t736_ref24_hz(ref, 26000000) == 24000000);
    assert(t736_ref24_hz(0xf8675f00, 26000000) == 24000000);
    assert(t736_peri0_800_hz(0xf8675f00, 0xff126310, 0, 0x30000000, 26000000) == 800000000);
    assert(t736_ref24_hz(ref, 24000000) == 0);
    assert(t736_ref24_hz(ref ^ BIT(16), 26000000) == 0);
    assert(t736_peri0_800_hz(ref, peri, 0, 0x30000000, 26000000) == 800000000);
    /* Direct DCXO gives 866.666..M, not 800M: it must be rejected. */
    assert(t736_peri0_800_hz(ref, peri & ~BIT(24), 0, 0, 26000000) == 0);
    assert(t736_peri0_800_hz(ref, peri, BIT(31), 0, 26000000) == 0);
    assert(t736_peri0_800_hz(ref, peri, 0, BIT(27), 26000000) == 0);
    assert(t736_peri0_800_hz(ref, peri, 0, BIT(26), 26000000) == 0);
    assert(t736_peri0_800_hz(ref, peri ^ BIT(16), 0, 0, 26000000) == 0);
    for (unsigned bit = 0; bit < 32; bit++) {
        if ((BIT(31) | BIT(28) | BIT(27)) & BIT(bit))
            assert(t736_ref24_hz(ref & ~BIT(bit), 26000000) == 0);
        if ((BIT(31) | BIT(28) | BIT(26)) & BIT(bit))
            assert(t736_peri0_800_hz(ref, peri & ~BIT(bit), 0, 0, 26000000) == 0);
    }
    unsigned targets[] = {400000, 12000000, 25000000, 50000000};
    for (unsigned k = 0; k < sizeof(targets) / sizeof(targets[0]); k++) {
        unsigned target = targets[k], parent = target > 12000000 ? 800000000 : 0;
        assert(t736_plan_sdr_clock(target, 24000000, parent, &p) == 0);
        assert(p.card_hz == target);
        assert(p.source == (parent ? 1U : 0U));
        assert(p.parent_hz == (parent ? parent : 24000000));
        assert(p.m >= 1 && p.m <= 32 && p.n >= 1 && p.n <= 32);
        assert(p.parent_hz == (u64)p.card_hz * 2 * p.m * p.n);
        assert(((p.reg >> 8) & 31) + 1 == p.n);
        assert((p.reg & 31) + 1 == p.m);
    }
    /* Sweep adversarial requests across both delay bands; the rational
     * rate, not its rounded integer representation, must stay below cap.
     */
    for (unsigned target = 400000; target <= 50000000; target += 1543) {
        assert(t736_plan_sdr_clock(target, 24000000, 800000000, &p) == 0);
        assert((u64)p.parent_hz <= (u64)target * 2 * p.m * p.n);
        for (unsigned n = 1; n <= 32; n++) {
            for (unsigned m = 1; m <= 32; m++) {
                unsigned parents[] = {24000000, 800000000};
                for (unsigned s = 0; s < 2; s++)
                    if ((u64)parents[s] <= (u64)target * 2 * m * n)
                        assert(parents[s] / (2 * m * n) <= p.card_hz);
            }
        }
    }
    assert(t736_plan_sdr_clock(399999, 24000000, 800000000, &p) == -EINVAL);
    assert(t736_plan_sdr_clock(52000000, 24000000, 800000000, &p) == -EINVAL);
    assert(t736_plan_sdr_clock(25000000, 26000000, 800000000, &p) == -EINVAL);
    assert(t736_plan_sdr_clock(25000000, 24000000, 866666666, &p) == -EINVAL);
    puts("PASS: exact REF/PERI source checks, active SDM rejection, linear divisors and no overspeed across 32,146 requests");
}
'''

with tempfile.TemporaryDirectory(prefix="t736-hs-clock-") as temporary:
    root = Path(temporary)
    source = root / "test.c"
    binary = root / "test"
    source.write_text(SOURCE)
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1",
                    "-fsanitize=address,undefined", "-I", str(ROOT / "boards"),
                    str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
