#!/usr/bin/env python3
"""Compile the actual A55 trial sequence with a deterministic host MMIO model.

This validates register sequencing/failure containment, not silicon behavior.
No Linux module is loaded and no real MMIO is accessed.
"""
from pathlib import Path
import subprocess
import tempfile

source = Path(__file__).with_name("t736-a55-clock-trial-v1.c").read_text()
defines = source[source.index("#define L_CTRL"):source.index("static char *action")]
sequence = source[source.index("static int write_mux"):source.index("static int guards_check")]
model = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
typedef uint32_t u32;
#define __iomem
#define BIT(n) (1U << (n))
#define GENMASK(h,l) (((~0U) << (l)) & ((~0U) >> (31-(h))))
#define pr_info(...) ((void)0)
#define pr_warn(...) ((void)0)
#define pr_crit(...) ((void)0)
#define udelay(x) ((void)(x))
static uint32_t words[0x4000/4], shadow_n;
static void *mmio = words;
static int early, fault, mutation, ctrl_writes;
enum { NONE, BAD_N, STUCK_UPDATE, NO_LOCK, FAIL_PLL_MUX };
static u32 readl(const void *p) { return *(const u32 *)p; }
static void writel(u32 value, void *p) {
    uintptr_t off = (char *)p - (char *)mmio;
    if (off == 0x101c) {
        assert(value == 0 || value == 0x03000000);
        if (fault == FAIL_PLL_MUX && value) value = 0;
        *(u32 *)p = value;
        return;
    }
    assert(off == 0x1000);
    assert(value & BIT(31)); /* PLL_EN must NEVER be disabled. */
    ctrl_writes++;
    u32 before = *(u32 *)p, n = before & 0xff00;
    shadow_n = value & 0xff00;
    if (value & BIT(26)) {
        assert(shadow_n == 0x2700); /* No re-commit of inherited N19. */
        n = shadow_n;
        if (fault != STUCK_UPDATE) value &= ~BIT(26);
    } else if (early && !(before & BIT(29)) && (value & BIT(29))) {
        n = shadow_n;
    }
    if (fault == BAD_N && shadow_n == 0x2700 && !(value & BIT(29)) && !mutation++)
        n = 0x2800;
    value = (value & ~(0xff00 | BIT(28))) | n;
    if ((value & BIT(29)) && fault != NO_LOCK) value |= BIT(28);
    words[0x1000/4] = value;
    words[0x102c/4] = value;
}
'''
cases = r'''
static void reset_case(int e, int f) {
    for (unsigned int i=0;i<sizeof(words)/sizeof(words[0]);i++) words[i]=0;
    words[L_CTRL/4]=words[L_STAT/4]=CTRL_494;
    words[L_CLK/4]=CLK_PLL;
    shadow_n=0x1300; early=e; fault=f; mutation=ctrl_writes=0;
}
int main(void) {
    for (int e=0;e<2;e++) {
        reset_case(e, NONE);
        assert(upgrade_1014(mmio)==0);
        assert(words[L_CTRL/4]==CTRL_1014 && words[L_STAT/4]==CTRL_1014);
        assert(words[L_CLK/4]==CLK_PLL);
        printf("PASS success early_N=%d\n",e);
        for (int f=BAD_N;f<=FAIL_PLL_MUX;f++) {
            reset_case(e,f);
            assert(upgrade_1014(mmio)<0);
            int writes_before=ctrl_writes;
            assert(safe_slow(mmio)==0);
            assert(ctrl_writes==writes_before); /* Fallback only writes CLK. */
            assert(words[L_CLK/4]==0 && (words[L_CTRL/4]&ENABLE));
            printf("PASS failure=%d early_N=%d EN retained; slow verified\n",f,e);
        }
    }
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="t736-a55-model-") as directory:
    directory = Path(directory)
    c = directory / "model.c"
    binary = directory / "model"
    c.write_text(model + defines + sequence + cases)
    subprocess.run(["cc", "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror",
                    str(c), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
