#!/usr/bin/env python3
"""Compile the CMD21 recovery classifier and unchanged normal DMA policy."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <stdio.h>
typedef uint32_t u32;
typedef uint64_t u64;
typedef uint32_t __le32;
#define BIT(n) (1U << (n))
#define DIV_ROUND_UP(a,b) (((a)+(b)-1)/(b))
#define min(a,b) ((a)<(b)?(a):(b))
#define cpu_to_le32(x) (x)
#define le32_to_cpu(x) (x)
#define READ_ONCE(x) (x)
#define dma_rmb() do {} while (0)
#include "t736_mmc_dma_desc.h"
#include "t736_mmc_tuning_error_draft.h"
#include "t736_mmc_tuning_draft.h"

int main(void)
{
    struct t736_dma_desc desc = {0};
    struct t736_tuning_window window;
    const u32 clean_irq = BIT(2) | BIT(3) | BIT(5);
    const u32 bad_irq = 0xac, bad_idst = 0x22;
    assert(!t736_tuning_idst_invalid(BIT(1)));
    assert(!t736_tuning_idst_invalid(bad_idst));
    assert(t736_tuning_error_recoverable(-EILSEQ, bad_irq, bad_idst));
    /* The same event remains an error for every actual data request. */
    assert(t736_dma_poll_result(bad_irq, bad_idst, true, false, false, &desc, 1) == -EILSEQ);
    assert(t736_dma_poll_result(clean_irq, bad_idst, true, false, false, &desc, 1) == -EIO);
    assert(!t736_tuning_error_recoverable(-EIO, clean_irq, bad_idst));
    assert(!t736_tuning_error_recoverable(-EILSEQ, clean_irq, bad_idst));
    assert(!t736_tuning_error_recoverable(-ETIMEDOUT, 0, BIT(5)));
    assert(t736_tuning_error_recoverable(-EIO, clean_irq, BIT(1)));
    assert(!t736_tuning_error_recoverable(-EIO, BIT(2), BIT(1)));
    assert(!t736_tuning_error_recoverable(-EIO, clean_irq, 0));
    assert(!t736_tuning_error_recoverable(-EIO, bad_irq, bad_idst));
    assert(!t736_tuning_error_recoverable(-EILSEQ, clean_irq, BIT(1)));
    assert(t736_tuning_error_recoverable(-ETIMEDOUT, 0, 0));
    assert(!t736_tuning_error_recoverable(-ENOMEM, bad_irq, bad_idst));
    assert(!t736_tuning_error_recoverable(0, bad_irq, bad_idst));
    for (unsigned bit = 0; bit < 32; bit++) {
        if (!(BIT(bit) & T736_TUNING_IDST_ALLOWED)) {
            /* Poll guard runs even if normal DMA policy reports success. */
            assert(t736_tuning_idst_invalid(BIT(1) | BIT(bit)));
            assert(!t736_tuning_error_recoverable(-EILSEQ, bad_irq, bad_idst | BIT(bit)));
        }
        if (BIT(bit) & T736_TUNING_CARD_ERRORS) {
            int err = BIT(bit) & (BIT(8) | BIT(9)) ? -ETIMEDOUT : -EILSEQ;
            assert(t736_tuning_error_recoverable(err, clean_irq | BIT(bit), bad_idst));
            assert(t736_tuning_error_recoverable(err, clean_irq | BIT(bit), BIT(1)));
        }
        if (BIT(bit) & (T736_IRQ_ERRORS & ~T736_TUNING_CARD_ERRORS)) {
            assert(!t736_tuning_error_recoverable(-EILSEQ, bad_irq | BIT(bit), bad_idst));
            assert(!t736_tuning_error_recoverable(-ETIMEDOUT, BIT(bit), 0));
        }
    }
    /* Reproduce taps0..31 good, taps32..63 bad: classify errors, never
     * count a failed tap, and select the verified linear midpoint15.
     */
    u64 passing = 0;
    for (unsigned tap = 0; tap < 64; tap++) {
        if (tap < 32)
            passing |= 1ULL << tap;
        else
            assert(t736_tuning_error_recoverable(-EILSEQ, bad_irq, bad_idst));
    }
    assert(passing == 0xffffffffULL);
    assert(!t736_tuning_select(passing, &window));
    assert(window.first == 0 && window.count == 32 && window.tap == 15);
    puts("PASS: observed0xac/0x22 classified bad tap; normalI/O still fails; unexplained summary/hard/unknown bits rejected; partial eye center15");
}
'''

with tempfile.TemporaryDirectory(prefix="t736-hs200-error-") as temporary:
    root = Path(temporary)
    source, binary = root / "test.c", root / "test"
    source.write_text(SOURCE)
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1",
                    "-fsanitize=address,undefined", "-I", str(ROOT / "boards"),
                    str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
