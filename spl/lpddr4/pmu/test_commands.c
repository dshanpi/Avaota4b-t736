/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "commands.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "command_vectors.h"
#include "inferno_encoder_extract.h"

static void check_cs(const struct t736_lp4_command *command, unsigned int rank,
                      unsigned int cycles)
{
    unsigned int i;
    assert(command->cycles == cycles);
    for (i = 0; i < 4u; ++i) {
        unsigned int expected = i < cycles && (i & 1u) == 0u ? 1u << rank : 0u;
        assert(command->cs_mask[i] == expected);
        assert(command->ca[i] < 64u);
    }
}

static void test_lp4(void)
{
    unsigned int rank, mr, op;
    struct t736_lp4_command command, before;
    for (rank = 0; rank < 2u; ++rank) {
        for (mr = 0; mr < 64u; ++mr) {
            for (op = 0; op < 256u; ++op) {
                assert(t736_lp4_encode_mrw(rank, mr, op, &command) == 0);
                assert(memcmp(command.ca, lp4_mrw[mr * 256u + op], 4) == 0);
                check_cs(&command, rank, 4);
            }
            assert(t736_lp4_encode_mrr(rank, mr, &command) == 0);
            assert(memcmp(command.ca, lp4_mrr[mr], 4) == 0);
            check_cs(&command, rank, 4);
        }
        for (op = 0; op < 128u; ++op) {
            int allowed = op < 64u || op == 0x41u || op == 0x43u || op == 0x47u ||
                          op == 0x4bu || op == 0x4du || op == 0x4fu || op == 0x51u;
            memset(&command, 0xa5, sizeof(command));
            before = command;
            if (!allowed) {
                assert(t736_lp4_encode_mpc(rank, op, &command) == -1);
                assert(memcmp(&command, &before, sizeof(command)) == 0);
            } else {
                assert(t736_lp4_encode_mpc(rank, op, &command) == 0);
                assert(memcmp(command.ca, lp4_mpc[op], 2) == 0);
                assert(command.ca[2] == 0 && command.ca[3] == 0);
                check_cs(&command, rank, 2);
            }
        }
    }
    memset(&command, 0xa5, sizeof(command));
    before = command;
    assert(t736_lp4_encode_mrw(0, 64, 0, &command) == -1);
    assert(t736_lp4_encode_mrw(0, 0, 256, &command) == -1);
    assert(t736_lp4_encode_mrw(2, 0, 0, &command) == -1);
    assert(t736_lp4_encode_mrw(0, 0, 0, NULL) == -1);
    assert(t736_lp4_encode_mrr(0, 64, &command) == -1);
    assert(t736_lp4_encode_mrr(2, 0, &command) == -1);
    assert(t736_lp4_encode_mpc(0, 128, &command) == -1);
    assert(t736_lp4_encode_mpc(2, 0, &command) == -1);
    assert(memcmp(&command, &before, sizeof(command)) == 0);
}

static void test_public_type9_ca_fields(void)
{
    unsigned int mr, op;
    u16 phase0, phase1;
    for (mr = 0; mr < 128u; ++mr) {
        for (op = 0; op < 256u; ++op) {
            const unsigned char *expected = lp5_mrw[mr * 256u + op];
            pmu_cal_encode_opcode_pair(6, op, mr, 0, &phase0, &phase1);
            assert(phase0 == expected[0] && phase1 == expected[1]);
            pmu_cal_encode_opcode_pair(6, op, mr, 1, &phase0, &phase1);
            assert(phase0 == expected[2] && phase1 == expected[3]);
        }
        pmu_cal_encode_opcode_pair(15, 0, mr, 0, &phase0, &phase1);
        assert(phase0 == lp5_mrr[mr][0] && phase1 == lp5_mrr[mr][1]);
    }
    for (op = 0; op < 256u; ++op) {
        pmu_cal_encode_opcode_pair(25, op, 0, 0, &phase0, &phase1);
        assert(phase0 == lp5_mpc[op][0] && phase1 == lp5_mpc[op][1]);
    }
}

int main(void)
{
    test_lp4();
    test_public_type9_ca_fields();
    puts("PASS: LP4 MRW/MRR/MPC wire vectors; LP5 descriptor CA fields independently matched");
    return 0;
}
