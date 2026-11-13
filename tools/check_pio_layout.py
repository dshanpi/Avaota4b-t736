#!/usr/bin/env python3
"""Host-run actual sunxi register helpers against independent register vectors.

Usage: check_pio_layout.py PATCHED_LINUX_TREE [C_COMPILER]
This does not access hardware. The vectors cover T736 eMMC/RGMII/last-bank
registers, plus legacy, D1 and the unchanged R-PIO layout.
"""
from pathlib import Path
import subprocess
import sys
import tempfile

source = Path(sys.argv[1]) / 'drivers/pinctrl/sunxi'
compiler = sys.argv[2] if len(sys.argv) > 2 else 'cc'
board = Path(__file__).resolve().parents[1] / 'boards/pinctrl-t736.c'
c = (source / 'pinctrl-sunxi.c').read_text()
helpers = c[c.index('static u32 sunxi_bank_offset('):
            c.index('static struct sunxi_pinctrl_group *')]
init = c[c.index('\tpctl->dlevel_regs_offset = DLEVEL_REGS_OFFSET;'):
         c.index('\tif (flags & SUNXI_PINCTRL_ELEVEN_BANKS)')]
b = board.read_text()
layout = b[b.index('static const struct sunxi_pinctrl_reg_layout'):
           b.index('static struct sunxi_pinctrl_desc')]
stub = r'''
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int refcount_t;
typedef int raw_spinlock_t;
#define __iomem
#define BIT(n) (1U << (n))
#define BITS_PER_TYPE(t) (sizeof(t) * 8)
struct pinctrl_pin_desc { unsigned number; const char *name; };
struct platform_device;
'''
harness = r'''
#include <assert.h>
#include <stdio.h>
static unsigned checks;
static void expect(u32 actual, u32 expected)
{
    if (actual != expected) {
        fprintf(stderr, "check %u: got 0x%x, expected 0x%x\n",
                checks, actual, expected);
        assert(actual == expected);
    }
    checks++;
}
static void check_reg(struct sunxi_pinctrl *pctl, u32 pin,
                     void (*fn)(const struct sunxi_pinctrl *, u32, u32 *, u32 *, u32 *),
                     u32 expected_reg, u32 expected_shift, u32 expected_mask)
{
    u32 reg, shift, mask;
    fn(pctl, pin, &reg, &shift, &mask);
    expect(reg, expected_reg);
    expect(shift, expected_shift);
    expect(mask, expected_mask);
}
int main(void)
{
    static const unsigned int map[] = {1,2,3,4,5,6,7,8,9,10};
    struct sunxi_pinctrl_desc desc = {
        .reg_layout = &t736_pio_layout, .irq_bank_map = map,
    };
    struct sunxi_pinctrl pctl = {.desc = &desc};
    /* Register list: PC_CFG0/1/2=180/184/188, PH_CFG0=400, PK_CFG0=580. */
    static const u32 mmc[][4] = {
        {0,0x180,0,0xf}, {1,0x180,4,0xf0}, {5,0x180,20,0x00f00000},
        {6,0x180,24,0x0f000000}, {8,0x184,0,0xf}, {9,0x184,4,0xf0},
        {10,0x184,8,0xf00}, {11,0x184,12,0xf000},
        {13,0x184,20,0x00f00000}, {14,0x184,24,0x0f000000},
        {15,0x184,28,0xf0000000}, {16,0x188,0,0xf},
    };
    unsigned i;
    init_layout(&pctl, &desc, 0);
    for (i=0; i<sizeof(mmc)/sizeof(mmc[0]); i++)
        check_reg(&pctl, PC_BASE+mmc[i][0], sunxi_mux_reg,
                  mmc[i][1], mmc[i][2], mmc[i][3]);
    check_reg(&pctl, PH_BASE+16, sunxi_data_reg, 0x410,16,0x10000);
    check_reg(&pctl, PH_BASE+16, sunxi_dlevel_reg, 0x428,0,0xf);
    check_reg(&pctl, PH_BASE+16, sunxi_pull_reg, 0x434,0,3);
    check_reg(&pctl, PK_BASE+25, sunxi_mux_reg, 0x58c,4,0xf0);
    expect(sunxi_irq_cfg_reg(&desc, 6*32+16), 0x448);
    expect(sunxi_irq_ctrl_reg_from_bank(&desc, 6), 0x450);
    expect(sunxi_irq_status_reg_from_bank(&desc, 6), 0x454);
    expect(sunxi_irq_debounce_reg_from_bank(&desc, 6), 0x458);
    expect(sunxi_irq_cfg_reg(&desc, 9*32+25), 0x5cc);
    expect(sunxi_irq_status_reg_from_bank(&desc, 9), 0x5d4);
    /* Legacy and D1 behavior must remain the same, including relocated PK. */
    desc.reg_layout = NULL;
    init_layout(&pctl, &desc, 0);
    check_reg(&pctl, PC_BASE+6, sunxi_mux_reg, 0x48,24,0x0f000000);
    check_reg(&pctl, PB_BASE+8, sunxi_dlevel_reg, 0x38,16,0x30000);
    check_reg(&pctl, PB_BASE+16, sunxi_pull_reg, 0x44,0,3);
    init_layout(&pctl, &desc, SUNXI_PINCTRL_NEW_REG_LAYOUT);
    check_reg(&pctl, PC_BASE+6, sunxi_mux_reg, 0x60,24,0x0f000000);
    check_reg(&pctl, PK_BASE+25, sunxi_mux_reg, 0x50c,4,0xf0);
    expect(sunxi_irq_cfg_reg(&desc, 6*32+16), 0x2e8);
    /* R-PIO helpers receive pins relative to PL_BASE. */
    desc.irq_bank_map = NULL;
    check_reg(&pctl, 6, sunxi_data_reg, 0x10,6,0x40);
    check_reg(&pctl, 32+5, sunxi_mux_reg, 0x30,20,0x00f00000);
    check_reg(&pctl, 32+5, sunxi_dlevel_reg, 0x44,20,0x00f00000);
    check_reg(&pctl, 32+5, sunxi_pull_reg, 0x54,10,0xc00);
    expect(sunxi_irq_ctrl_reg_from_bank(&desc, 1), 0x230);
    expect(sunxi_irq_status_reg_from_bank(&desc, 1), 0x234);
    expect(sunxi_irq_debounce_reg_from_bank(&desc, 1), 0x238);
    printf("PASS: %u register/shift/mask checks; T736 main PIO, R-PIO, legacy and D1\n", checks);
}
'''
with tempfile.TemporaryDirectory(prefix='t736-pio-check-') as tmp:
    d = Path(tmp)
    (d / 'linux').mkdir()
    (d / 'linux/kernel.h').write_text(stub)
    (d / 'linux/spinlock.h').write_text('')
    program = '#include "pinctrl-sunxi.h"\n' + helpers + layout
    program += ('\nstatic void init_layout(struct sunxi_pinctrl *pctl, '
                'const struct sunxi_pinctrl_desc *desc, unsigned long flags)\n{\n'
                + init + '}\n' + harness)
    (d / 'test.c').write_text(program)
    subprocess.run([compiler, '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-I', str(d), '-I', str(source), str(d / 'test.c'),
                    '-o', str(d / 'test')], check=True)
    subprocess.run([str(d / 'test')], check=True)
