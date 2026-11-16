#!/usr/bin/env python3
"""Actual C boundaries for HS200 parent math and a non-wrapping tuning eye."""
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
#include "t736_mmc_clock_hs200_draft.h"
#include "t736_mmc_tuning_draft.h"

int main(void)
{
    struct t736_tuning_window w;
    struct t736_clock_plan p;
    assert(t736_peri0_800_hz(0xf8675f00, 0xff126310, 0, 0x30000000, 26000000) == 800000000);
    assert(t736_plan_sdr_clock(100000000, 24000000, 800000000, &p) == 0);
    assert(p.card_hz == 100000000 && p.source == 1 && p.m == 4 && p.n == 1);
    assert(t736_plan_sdr_clock(200000000, 24000000, 800000000, &p) == 0);
    assert(p.card_hz == 200000000 && p.source == 1 && p.m == 2 && p.n == 1);
    assert(t736_plan_sdr_clock(200000001, 24000000, 800000000, &p) == -EINVAL);
    assert(t736_tuning_select(0, &w) == -EIO);
    assert(t736_tuning_select(1ULL << 63, &w) == -EIO);
    assert(t736_tuning_select(0x7f, &w) == -EIO);
    assert(t736_tuning_select(0xf00000000000000fULL, &w) == -EIO);
    assert(t736_tuning_select(0x7ff, &w) == -EIO);
    assert(t736_tuning_select(0xfc0000000000003fULL, &w) == -EIO);
    assert(t736_tuning_select(0xfff, &w) == 0 && w.first == 0 && w.count == 12 && w.tap == 5);
    assert(t736_tuning_select(0x1fff, &w) == 0 && w.tap == 6);
    assert(t736_tuning_select(0xfff0000000000000ULL, &w) == 0 && w.first == 52 && w.tap == 57);
    assert(t736_tuning_select(UINT64_MAX, &w) == 0 && w.first == 0 && w.count == 64 && w.tap == 31);
    assert(t736_tuning_select(0xfff0000fffULL, &w) == 0 && w.first == 0 && w.tap == 5);
    assert(t736_tuning_select(0x1fff0000fffULL, &w) == 0 && w.first == 28 && w.count == 13 && w.tap == 34);
    for (unsigned first = 0; first < 64; first++) {
        for (unsigned count = 1; count <= 64 - first; count++) {
            u64 mask = count == 64 ? UINT64_MAX : ((1ULL << count) - 1) << first;
            int ret = t736_tuning_select(mask, &w);
            if (count < T736_TUNING_MIN_WINDOW) {
                assert(ret == -EIO);
            } else {
                assert(!ret && w.first == first && w.count == count);
                assert(w.tap - first >= 5 && first + count - 1 - w.tap >= 5);
                assert(mask & (1ULL << w.tap));
            }
        }
    }
    assert(t736_hs200_seed(25000000) == 0);
    assert(t736_hs200_seed(25000001) == 32);
    assert(t736_hs200_seed(100000000) == 32);
    assert(t736_hs200_seed(100000001) == 40);
    assert(t736_hs200_seed(150000000) == 40);
    assert(t736_hs200_seed(150000001) == 49);
    assert(t736_hs200_seed(200000000) == 49);
    puts("PASS: exact100/200MHz, all2080linear intervals, no endpoint-wrap, minimum margins, actual-board seed boundaries");
}
'''

with tempfile.TemporaryDirectory(prefix="t736-hs200-") as temporary:
    root = Path(temporary)
    source, binary = root / "test.c", root / "test"
    source.write_text(SOURCE)
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1",
                    "-fsanitize=address,undefined", "-I", str(ROOT / "boards"),
                    str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
