/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef T736_LP4_CONTROLLER_STATIC_V2_H
#define T736_LP4_CONTROLLER_STATIC_V2_H
#include "host.h"

#define T736_INIT_V2_WORDS 6u
enum t736_init_v2_result {
 T736_INIT_V2_CONFIGURED_ONLY=1,
 T736_INIT_V2_INVALID=-1,
 T736_INIT_V2_UNSUPPORTED=-2,
 T736_INIT_V2_IO=-3,
 T736_INIT_V2_PRECONDITION=-4,
 T736_INIT_V2_VERIFY=-5,
 T736_INIT_V2_TIMEOUT=-6,
 T736_INIT_V2_TIMER=-7
};
/* Whole-microsecond waits only: quantum is1024 counter cycles.
 * Reject overflow/field truncation; output unchanged on failure. */
int t736_init_v2_us_to_x1024(uint32_t us,uint32_t counter_mhz,
                        uint32_t width,uint32_t minimum,uint32_t *out);
/* LP4 setA/DBI-off latency rows; BL16, 2CK write preamble, toggling read
 * preamble and1.5CK read postamble. Reject inconsistent latency rows. */
int t736_init_v2_encode_latency(uint32_t rl,uint32_t wl,uint32_t nwr,
                            uint32_t *mr1,uint32_t *mr2);
struct t736_init_v2_profile {
 uint32_t ck_mhz,counter_mhz,rl,wl,nwr;
 /* reset_low_min_us=200 is an external measured-PHY-reset requirement,
  * not a controller register value or a wait performed by this component. */
 uint32_t pre_cke_ticks,post_cke_ticks,reset_low_min_us;
 /* MR1,2,3,13,11,12,22,14; selected electrical seeds are not training. */
 uint32_t mr[8];
 /* CTL INITTMG0; TIMING INITMR0..3,TMGCFG bit0. INITTMG1 is not written. */
 uint32_t value[T736_INIT_V2_WORDS];
};
/* Fixed existing1800MHz/RL36/WL18/nWR40 profile. counter_mhz must explicitly
 * be450: this is the accepted T736 host-interface assumption, not a generic
 * interpretation of every Synopsys controller's counter clock. */
int t736_init_v2_build_profile(uint32_t ck_mhz,uint32_t counter_mhz,
                           struct t736_init_v2_profile *out);

struct t736_init_v2_report {
 uint32_t stage,reads_attempted,reads_completed,writes_attempted,writes_completed;
 uint32_t last_operation,last_address,last_value;
 uint32_t swctl_maybe_open,static_maybe_open,configured_channels,final_valid;
 uint32_t waited_us;
 uint32_t before[2][T736_INIT_V2_WORDS],programmed[2][T736_INIT_V2_WORDS];
 uint32_t inittmg1_before[2],inittmg1_after[2];
};
/* Source-only controlled experiment, not a DRAM-init API.
 * Requires the exact cold held state from controller_init_read_probe, with
 * expected_micro exactly1 or9 selected/frozen by the hardware owner.
 * All preflights precede mutation. Performs20 word writes: both SWCTL open,
 * both SWCTLSTATIC unlock, six fields/channel, relock and SWCTL commit.
 * INITTMG1 is read-only here and must be exactly0 before and after.
 * RESET_n low timing is an external source PHY operation, not this API.
 * Preserves unrelated INITTMG0/TMGCFG bits. Never writes the existing64-word
 * subset, MRCTRL, DFI controls, PWRCTL, PHY, DRAM contents or storage.
 * Callback failure stops immediately without speculative cleanup/retry.
 * A failed write may have taken effect: maybe_open masks retain uncertainty.
 * Callbacks must themselves terminate; an APB bus hang is not bounded here.
 */
int t736_init_v2_apply_static(const struct t736_phy_io *io,uint32_t ck_mhz,
                          uint32_t counter_mhz,uint32_t expected_micro,
                          uint32_t timeout_us,struct t736_init_v2_report *report);
#endif
