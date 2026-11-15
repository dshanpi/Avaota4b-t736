/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "common_message.h"

int t736_lp4_patch_common(uint8_t *message, size_t length,
                         const struct t736_lp4_common_config *config)
{
    static const uint8_t width_offset[2] = {0x24, 0x57};
    static const uint8_t rank_offset[2] = {0x25, 0x58};
    static const uint8_t mr_offset[2][2] = {{0x32, 0x3e}, {0x65, 0x71}};
    unsigned int channel, rank, mr;

    if (!message || !config || length < T736_LP4_COMMON_END ||
        config->data_rate_mts == 0u ||
        (config->sequence & T736_LP4_SEQ_DEVICE_INIT) == 0u ||
        (config->sequence & ~T736_LP4_SEQ_VALID) != 0u)
        return -1;
    for (channel = 0; channel < 2u; ++channel) {
        uint8_t ranks = config->rank_mask[channel];
        if (channel == 1u && ranks == 0u && config->dq_width[channel] == 0u)
            continue;
        if ((ranks != 1u && ranks != 3u) || config->dq_width[channel] != 16u)
            return -1;
    }
    message[0x06] = (uint8_t)config->data_rate_mts;
    message[0x07] = (uint8_t)(config->data_rate_mts >> 8);
    message[0x10] = (uint8_t)config->sequence;
    message[0x11] = (uint8_t)(config->sequence >> 8);
    for (channel = 0; channel < 2u; ++channel) {
        message[width_offset[channel]] = config->dq_width[channel];
        message[rank_offset[channel]] = config->rank_mask[channel];
        for (rank = 0; rank < 2u; ++rank)
            for (mr = 0; mr < T736_LP4_MR_COUNT; ++mr)
                message[mr_offset[channel][rank] + mr] =
                    config->mode_register[channel][rank][mr];
    }
    return 0;
}
