/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef T736_CPUCLOCK_C_H
#define T736_CPUCLOCK_C_H
#include "cpuclock.h"
struct t736_cpuclock_c_trace {
 uint32_t step, reg, before, desired, actual, ctrl, stat, clk;
 int error;
};
typedef void (*t736_cpuclock_c_trace_fn)(void *, const struct t736_cpuclock_c_trace *);
struct t736_cpuclock_c_report {
 struct t736_cpuclock_report base;
 struct t736_cpuclock_c_trace last, failed;
 uint32_t failure_recorded, trace_count;
 t736_cpuclock_c_trace_fn trace;
};
/* Exact two-part runtime-v4 path: cold N32/ext2 then warm N39/ext1.
 * B1/B2 remain frozen. This new candidate requires independent cold boot.
 * Callback only observes captured values; it must not write CPU or PMIC state.
 */
int t736_cpuclock_c_prepare(const struct t736_cpuclock_io *,
 struct t736_cpuclock_c_report *, t736_cpuclock_c_trace_fn);
#endif
