#!/usr/bin/env python3
"""Run the actual DT function-table builder with empty DT children on the host."""
from pathlib import Path
import subprocess
import sys
import tempfile

src = Path(sys.argv[1]) / 'drivers/pinctrl/sunxi/pinctrl-sunxi-dt.c'
text = src.read_text()
start = text.index('static int prepare_function_table(')
end = text.index('\n}\n', start) + 3
function = text[start:end]
stub = r'''
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <errno.h>
typedef uint8_t u8;
#define PINS_PER_BANK 32
#define GFP_KERNEL 0
struct device { int unused; };
struct device_node { int unused; };
struct property { int unused; };
struct sunxi_desc_function {
    unsigned long variant;
    const char *name;
    u8 muxval, irqbank, irqnum;
};
struct sunxi_desc_pin {
    struct { unsigned number; const char *name; } pin;
    unsigned long variant;
    struct sunxi_desc_function *functions;
};
#define devm_kcalloc(dev, n, size, flags) calloc(n, size)
#define for_each_child_of_node(pnode, node) for (node = NULL; node; node = NULL)
#define of_property_for_each_string(node, key, prop, name) \
    for (prop = NULL, name = NULL; prop && name; prop = NULL)
'''
test = r'''
static unsigned checks;
static void run(unsigned base, const u8 *counts, const u8 *irqs, int nbanks)
{
    struct sunxi_desc_pin pins[352] = {0};
    unsigned p = 0, bank, n, irq_index = 0;
    for (bank = 0; bank < (unsigned)nbanks; bank++)
        for (n = 0; n < counts[bank]; n++)
            pins[p++].pin.number = base + 32 * bank + n;
    assert(prepare_function_table(NULL, NULL, pins, p, base, irqs) == 0);
    p = 0;
    for (bank = 0; bank < (unsigned)nbanks; bank++) {
        for (n = 0; n < counts[bank]; n++, p++) {
            struct sunxi_desc_function *f = pins[p].functions;
            assert(!strcmp(f[0].name, "gpio_in"));
            assert(!strcmp(f[1].name, "gpio_out"));
            if (irqs[bank]) {
                assert(!strcmp(f[2].name, "irq"));
                assert(f[2].irqbank == irq_index);
                assert(f[2].irqnum == n);
                assert(f[2].muxval == irqs[bank]);
                assert(f[3].name == NULL);
            } else {
                assert(f[2].name == NULL);
            }
            checks++;
        }
        if (counts[bank] && irqs[bank]) irq_index++;
    }
    free(pins[0].functions);
}
int main(void)
{
    const u8 main_counts[] = {0,11,17,24,16,7,15,20,17,28,26};
    const u8 main_irqs[] = {0,14,14,14,14,14,14,14,14,14,14};
    const u8 r_counts[] = {14,6}, r_irqs[] = {14,14};
    const u8 gaps_counts[] = {4,3,0,6,2}, gaps_irqs[] = {0,6,0,0,6};
    run(0, main_counts, main_irqs, 11);
    run(352, r_counts, r_irqs, 2);
    run(0, gaps_counts, gaps_irqs, 5);
    printf("PASS: IRQ table for %u pins; absent PA, R-PIO and non-IRQ/gap banks\n", checks);
}
'''
with tempfile.TemporaryDirectory(prefix='t736-irq-test-') as tmp:
    d = Path(tmp)
    (d / 'test.c').write_text(stub + function + test)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-parameter', '-fsanitize=address,undefined',
                    str(d / 'test.c'), '-o', str(d / 'test')], check=True)
    subprocess.run([str(d / 'test')], check=True)
