/* SPDX-License-Identifier: BSD-2-Clause */
/* LPDDR4 pin encoding derived from LiteDRAM, Copyright (c) 2021 Antmicro.
 * See reference/LITEDRAM-LICENSE and command-source-manifest.json.
 */
#include "commands.h"

static int valid(unsigned int rank, const struct t736_lp4_command *out)
{
    return rank < 2u && out != 0;
}

static void cs_for_command(unsigned int rank, unsigned int cycles,
                           struct t736_lp4_command *out)
{
    out->cycles = (uint8_t)cycles;
    out->cs_mask[0] = (uint8_t)(1u << rank);
    out->cs_mask[1] = 0;
    out->cs_mask[2] = (uint8_t)(cycles == 4u ? 1u << rank : 0u);
    out->cs_mask[3] = 0;
}

int t736_lp4_encode_mrw(unsigned int rank, unsigned int mr,
                        unsigned int operand, struct t736_lp4_command *out)
{
    if (!valid(rank, out) || mr > 63u || operand > 255u)
        return -1;
    /* MRW-1: CA0..5 = L H H L L OP7, then MA0..5. */
    out->ca[0] = (uint8_t)(0x06u | ((operand >> 2) & 0x20u));
    out->ca[1] = (uint8_t)mr;
    /* MRW-2: CA0..5 = L H H L H OP6, then OP0..5. */
    out->ca[2] = (uint8_t)(0x16u | ((operand >> 1) & 0x20u));
    out->ca[3] = (uint8_t)(operand & 0x3fu);
    cs_for_command(rank, 4u, out);
    return 0;
}

int t736_lp4_encode_mrr(unsigned int rank, unsigned int mr,
                        struct t736_lp4_command *out)
{
    if (!valid(rank, out) || mr > 63u)
        return -1;
    out->ca[0] = 0x0e; /* MRR-1, V is driven low. */
    out->ca[1] = (uint8_t)mr;
    out->ca[2] = 0x12; /* CAS-2, C8 is zero for 6-bit MR address. */
    /* Match the public DFI adapter: its address input carries MA and is also
     * the CAS-2 C2..7 source. This encoder does not implement DQ reception.
     */
    out->ca[3] = (uint8_t)(mr >> 2);
    cs_for_command(rank, 4u, out);
    return 0;
}

int t736_lp4_encode_mpc(unsigned int rank, unsigned int opcode,
                        struct t736_lp4_command *out)
{
    if (!valid(rank, out) || opcode > 0x7fu)
        return -1;
    if (opcode >= 0x40u) {
        switch (opcode) {
        case T736_LP4_MPC_READ_FIFO:
        case T736_LP4_MPC_READ_DQ_CAL:
        case T736_LP4_MPC_WRITE_FIFO:
        case T736_LP4_MPC_START_DQS_OSC:
        case T736_LP4_MPC_STOP_DQS_OSC:
        case T736_LP4_MPC_ZQ_START:
        case T736_LP4_MPC_ZQ_LATCH:
            break;
        default:
            return -1; /* RFU operands are not commands. */
        }
    }
    out->ca[0] = (uint8_t)((opcode >> 1) & 0x20u);
    out->ca[1] = (uint8_t)(opcode & 0x3fu);
    out->ca[2] = 0;
    out->ca[3] = 0;
    cs_for_command(rank, 2u, out);
    return 0;
}
