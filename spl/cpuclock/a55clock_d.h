/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef T736_A55CLOCK_D_H
#define T736_A55CLOCK_D_H
#include "a55clock.h"
#define T736_A55_D_TRACES 24
struct t736_a55clock_d_trace {
 uint32_t step, reg, before, desired, actual, ctrl, stat, clk;
 int error;
};
struct t736_a55clock_d_report {
 struct t736_a55clock_report base;
 struct t736_a55clock_d_trace trace[T736_A55_D_TRACES], failed;
 uint32_t trace_count, failure_recorded;
};
/* Independent D candidate. All A55 trace capture stays in memory until return.
 * Only write-N and following LOCK_EN allow exactly original or requested N;
 * the explicit UPDATE and final CTRL/STAT/gates remain exact.
 */
int t736_a55clock_d_prepare(const struct t736_cpuclock_io *io,
                          struct t736_a55clock_d_report *report);
#endif
