/* SPDX-License-Identifier: BSD-2-Clause */
/* LPDDR4 pin encoding derived from LiteDRAM, Copyright (c) 2021 Antmicro.
 * See reference/LITEDRAM-LICENSE and command-source-manifest.json.
 */
#ifndef T736_LP4_COMMANDS_H
#define T736_LP4_COMMANDS_H

#include <stdint.h>

enum t736_lp4_mpc_opcode {
    T736_LP4_MPC_NOP = 0x00,
    T736_LP4_MPC_READ_FIFO = 0x41,
    T736_LP4_MPC_READ_DQ_CAL = 0x43,
    T736_LP4_MPC_WRITE_FIFO = 0x47,
    T736_LP4_MPC_START_DQS_OSC = 0x4b,
    T736_LP4_MPC_STOP_DQS_OSC = 0x4d,
    T736_LP4_MPC_ZQ_START = 0x4f,
    T736_LP4_MPC_ZQ_LATCH = 0x51
};

struct t736_lp4_command {
    uint8_t cycles;
    uint8_t ca[4];
    uint8_t cs_mask[4];
};

/* Output describes successive RISING CK edges, not DDR half-cycle phases.
 * CA uses logical CA[5:0] (no board swizzle); cs_mask bit N drives rank N.
 * Rank must be 0 or 1. All functions leave output unchanged on error.
 * Return 0 or -1 for invalid arguments. No I/O or execution is performed.
 *
 * Type9 phase-to-SDR-clock mapping, CS gate semantics and receive path are
 * not established. Do NOT directly copy these cycles into LP5 descriptors.
 * Caller must enforce tMRD/tMRW/tMPC/tZQCAL/tZQLAT and other state timings.
 * RESET_n/CKE and the CBT entry/exit sequence are separate physical actions.
 */
int t736_lp4_encode_mrw(unsigned int rank, unsigned int mr,
                        unsigned int operand, struct t736_lp4_command *out);
int t736_lp4_encode_mrr(unsigned int rank, unsigned int mr,
                        struct t736_lp4_command *out);
int t736_lp4_encode_mpc(unsigned int rank, unsigned int opcode,
                        struct t736_lp4_command *out);

#endif
