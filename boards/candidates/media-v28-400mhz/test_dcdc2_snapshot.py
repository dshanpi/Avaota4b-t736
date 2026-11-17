#!/usr/bin/env python3
"""Compile the actual sysfs callback with fake PMIC and lock checks; no device I/O."""
from pathlib import Path
import argparse
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--work', type=Path, required=True)
a = p.parse_args()
a.work.mkdir(parents=True, exist_ok=True)
source = (Path(__file__).parent / 't736-gpu-supply.c').read_text()
callback = source[source.index('static ssize_t dcdc2_snapshot_show('):
                  source.index('static DEVICE_ATTR_RO(dcdc2_snapshot);')]
assert '.dev_groups = t736_pmic_groups' in source
assert 'static DEVICE_ATTR_RO(dcdc2_snapshot);' in source
prefix = r'''
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
typedef uint8_t u8;
#define BIT(n) (1U << (n))
struct device { int identity; };
struct device_attribute { int unused; };
struct mutex { int identity; };
struct t736_gpu_supply { struct device *dev; struct mutex lock; };
static struct mutex cpu_supply_owner_lock = {1};
static struct t736_gpu_supply *cpu_supply_owner;
static struct device device = {1}, foreign_device = {2};
static struct t736_gpu_supply supply = {&device, {2}};
static unsigned stage, calls, warnings, emits;
static int fail_at;
static u8 registers[5];
static void mutex_lock(struct mutex *m) {
    assert(stage + 1 == (unsigned)m->identity); stage++;
}
static void mutex_unlock(struct mutex *m) {
    assert(stage == (unsigned)m->identity); stage--;
}
static int pmic_read(struct t736_gpu_supply *p, u8 reg, u8 *value) {
    static const u8 expected[] = {0x0e, 0x10, 0x13, 0x1b, 0x1d};
    assert(p == &supply && stage == 2 && calls < 5);
    assert(reg == expected[calls]);
    unsigned index = calls++;
    if ((int)index == fail_at) return -EIO;
    *value = registers[index]; return 0;
}
static void dev_warn_ratelimited(struct device *d, const char *fmt, ...) {
    assert(d == &device && stage == 0); (void)fmt; warnings++;
}
static int sysfs_emit(char *buf, const char *fmt, ...) {
    va_list ap; assert(stage == 0); emits++;
    va_start(ap, fmt); int rc = vsnprintf(buf, 4096, fmt, ap); va_end(ap);
    return rc;
}
static void reset(void) {
    assert(stage == 0); calls = warnings = emits = 0; fail_at = -1;
    registers[0] = 3; registers[1] = 2; registers[2] = 0x22;
    registers[3] = 0; registers[4] = 0;
    cpu_supply_owner = &supply;
}
'''
main = r'''
int main(void) {
    char out[4096]; ssize_t rc; unsigned tests = 0;
    reset(); cpu_supply_owner = NULL;
    assert(dcdc2_snapshot_show(&device, NULL, out) == -ENODEV);
    assert(!stage && !calls && !emits); tests++;
    reset();
    assert(dcdc2_snapshot_show(&foreign_device, NULL, out) == -ENODEV);
    assert(!stage && !calls && !emits); tests++;
    reset(); registers[0] = 4;
    assert(dcdc2_snapshot_show(&device, NULL, out) == -ENODEV);
    assert(!stage && calls == 1 && !emits); tests++;
    for (int failure = 0; failure < 5; failure++) {
        reset(); fail_at = failure;
        assert(dcdc2_snapshot_show(&device, NULL, out) == -EIO);
        assert(!stage && calls == (unsigned)failure + 1 && !emits); tests++;
    }
    for (unsigned raw = 0; raw < 256; raw++) {
        reset(); registers[2] = raw;
        rc = dcdc2_snapshot_show(&device, NULL, out);
        unsigned sel = raw & 0x7f;
        assert(!stage && calls == 5);
        if (sel > 0x57) {
            assert(rc == -ERANGE && !emits && warnings == 1);
        } else {
            unsigned uv = sel <= 0x46 ? 500000 + 10000 * sel :
                          1220000 + 20000 * (sel - 0x47);
            char expected[64]; snprintf(expected, sizeof(expected), "setpoint_uv=%u ", uv);
            assert(rc > 0 && emits == 1 && !warnings && strstr(out, expected));
            assert(strstr(out, raw & 0x80 ? "dvm=1 " : "dvm=0 "));
            assert(strstr(out, "remote_feedback=1 ") && strstr(out, "kind=programmed-setpoint"));
        }
        tests++;
    }
    reset(); registers[1] = 0;
    assert(dcdc2_snapshot_show(&device, NULL, out) == -ERANGE && !emits); tests++;
    reset(); registers[3] = BIT(6);
    assert(dcdc2_snapshot_show(&device, NULL, out) == -ERANGE && !emits); tests++;
    reset(); registers[4] = 1;
    assert(dcdc2_snapshot_show(&device, NULL, out) > 0 && strstr(out, "remote_feedback=0 ")); tests++;
    assert(!stage);
    printf("PASS: %u actual-callback cases; owner identity, lock ordering, five register reads, failure short-circuit, all 256 selector/DVM values, enable/phase guards, feedback; no PMIC write function linked\n", tests);
    return 0;
}
'''
test = a.work / 'dcdc2-snapshot-test.c'
test.write_text(prefix + callback + main)
binary = a.work / 'dcdc2-snapshot-test'
subprocess.run(['gcc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                str(test), '-o', str(binary)], check=True)
subprocess.run([str(binary)], check=True)
