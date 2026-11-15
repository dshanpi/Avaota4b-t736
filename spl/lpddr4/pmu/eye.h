/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef T736_LP4_EYE_H
#define T736_LP4_EYE_H

#include <stddef.h>
#include <stdint.h>

#define T736_LP4_MAX_TAPS 1024u

struct t736_lp4_eye {
    uint16_t first;
    uint16_t last;
    uint16_t center;
    uint16_t width;
};

/* Select the widest contiguous window in measured errors for one delay axis.
 * A sample passes when (errors[tap] & required_lanes) == 0. No interpolation,
 * wraparound or combining disconnected eyes. With equal widths use the lower
 * window. Endpoints are rejected if require_closed != 0 and the eye extends
 * beyond the scan. guard taps are removed from both edges before min_width.
 * On error, leaves *eye unchanged. -1 invalid; -2 no qualifying measured eye.
 * This is data reduction ONLY: it cannot measure DRAM or program PHY delays.
 */
int t736_lp4_eye_select(const uint32_t *errors, size_t count,
                        uint32_t required_lanes, uint16_t guard,
                        uint16_t min_width, int require_closed,
                        struct t736_lp4_eye *eye);

#endif
